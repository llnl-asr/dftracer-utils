// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/trace/index/extensions/kinds/payloads.h>
#include <dftracer/utils/trace/index/extensions/kinds/time_bounds.h>
#include <dftracer/utils/trace/index/tables.h>

#include <algorithm>
#include <cstdlib>
#include <system_error>

namespace dftracer::utils::trace::index::extensions::kinds {

namespace {
bool parse(const dstd::String& text, std::uint64_t& out) {
    const auto* end = text.data() + text.size();
    auto [ptr, ec] = dstd::from_chars(text.data(), end, out);
    return ec == std::errc() && ptr == end;
}

// Per chunk, the [min, max] of the zonemap of `path`.
dstd::Result<
    dstd::HashMap<std::uint64_t, dstd::Pair<std::uint64_t, std::uint64_t>>>
chunk_bounds(const trace::index::Snapshot& snapshot,
             const trace::index::FileRef& file, dstd::StringView path) {
    dstd::HashMap<std::uint64_t, dstd::Pair<std::uint64_t, std::uint64_t>> out;
    if (!trace::index::extension_current(
            snapshot, file, trace::index::store::IndexExtension::Zonemap))
        return out;
    const auto stored = trace::index::granules(
        snapshot, file, trace::index::store::IndexExtension::Zonemap, path);
    if (!stored) return stored.error();
    for (const auto& [granule, bytes] : *stored) {
        auto zone = decode_zone(bytes);
        std::uint64_t lo = 0;
        std::uint64_t hi = 0;
        if (zone && parse(zone->min, lo) && parse(zone->max, hi))
            out.emplace(granule,
                        dstd::Pair<std::uint64_t, std::uint64_t>{lo, hi});
    }
    return out;
}

}  // namespace

dstd::Result<
    dstd::HashMap<std::uint64_t, dstd::Pair<std::uint64_t, std::uint64_t>>>
chunk_time_bounds(const trace::index::Snapshot& snapshot,
                  const trace::index::FileRef& file) {
    return chunk_bounds(snapshot, file, "te");
}

dstd::Result<
    dstd::HashMap<std::uint64_t, dstd::Pair<std::uint64_t, std::uint64_t>>>
chunk_start_bounds(const trace::index::Snapshot& snapshot,
                   const trace::index::FileRef& file) {
    return chunk_bounds(snapshot, file, "ts");
}

dstd::Result<dstd::HashSet<std::uint64_t>> chunks_without_starts(
    const trace::index::Snapshot& snapshot, const trace::index::FileRef& file,
    std::uint64_t begin, std::uint64_t end) {
    dstd::HashSet<std::uint64_t> out;
    if (!trace::index::extension_current(
            snapshot, file, trace::index::store::IndexExtension::Zonemap))
        return out;
    const auto stored = trace::index::granules(
        snapshot, file, trace::index::store::IndexExtension::Zonemap, "ts");
    if (!stored) return stored.error();
    for (const auto& [granule, bytes] : *stored) {
        auto zone = decode_zone(bytes);
        if (zone && zone->histogram && !zone->histogram->empty() &&
            zone->histogram->count_in_range(begin, end) == 0)
            out.insert(granule);
    }
    return out;
}

dstd::Result<trace::index::gzip::TimeBounds> file_time_bounds(
    const trace::index::Snapshot& snapshot, const trace::index::FileRef& file) {
    trace::index::gzip::TimeBounds bounds;
    const auto chunks = chunk_time_bounds(snapshot, file);
    if (!chunks) return chunks.error();
    for (const auto& [granule, range] : *chunks) {
        if (range.first > range.second) continue;
        bounds.valid = true;
        bounds.min_timestamp_us =
            std::min(bounds.min_timestamp_us, range.first);
        bounds.max_timestamp_us =
            std::max(bounds.max_timestamp_us, range.second);
    }
    return bounds;
}

dstd::Result<dstd::HashMap<std::uint64_t, dstd::Pair<double, double>>>
chunk_value_ranges(const trace::index::Snapshot& snapshot,
                   const trace::index::FileRef& file, dstd::StringView path) {
    dstd::HashMap<std::uint64_t, dstd::Pair<double, double>> out;
    if (!trace::index::extension_current(
            snapshot, file, trace::index::store::IndexExtension::Zonemap))
        return out;
    auto number = [](const dstd::String& text, double& v) {
        char* end = nullptr;
        v = std::strtod(text.c_str(), &end);
        return !text.empty() && end == text.c_str() + text.size();
    };
    const auto stored = trace::index::granules(
        snapshot, file, trace::index::store::IndexExtension::Zonemap, path);
    if (!stored) return stored.error();
    for (const auto& [granule, bytes] : *stored) {
        auto zone = decode_zone(bytes);
        double lo = 0;
        double hi = 0;
        if (!zone || zone->value_type == "string" || !number(zone->min, lo) ||
            !number(zone->max, hi) || lo > hi)
            continue;
        out.emplace(granule, dstd::Pair<double, double>{lo, hi});
    }
    return out;
}

dstd::Result<dstd::Optional<dstd::Pair<double, double>>> file_value_range(
    const trace::index::Snapshot& snapshot, const trace::index::FileRef& file,
    dstd::StringView path) {
    dstd::Optional<dstd::Pair<double, double>> out;
    const auto ranges = chunk_value_ranges(snapshot, file, path);
    if (!ranges) return ranges.error();
    for (const auto& [granule, range] : *ranges) {
        out =
            out ? dstd::Pair<double, double>{std::min(out->first, range.first),
                                             std::max(out->second,
                                                      range.second)}
                : range;
    }
    return out;
}

}  // namespace dftracer::utils::trace::index::extensions::kinds
