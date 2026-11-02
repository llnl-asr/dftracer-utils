// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/index/extensions/kinds/payloads.h>
#include <dftracer/utils/index/extensions/kinds/time_bounds.h>
#include <dftracer/utils/index/tables.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>

namespace dftracer::utils::index::extensions::kinds {

namespace {
bool parse(const std::string& text, std::uint64_t& out) {
    const auto* end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(text.data(), end, out);
    return ec == std::errc() && ptr == end;
}

// Per chunk, the [min, max] of the zonemap of `path`.
ankerl::unordered_dense::map<std::uint64_t,
                             std::pair<std::uint64_t, std::uint64_t>>
chunk_bounds(const index::Snapshot& snapshot, const index::FileRef& file,
             std::string_view path) {
    ankerl::unordered_dense::map<std::uint64_t,
                                 std::pair<std::uint64_t, std::uint64_t>>
        out;
    if (!index::extension_current(snapshot, file,
                                  index::store::IndexExtension::Zonemap))
        return out;
    for (const auto& [granule, bytes] : common::unwrap(index::granules(
             snapshot, file, index::store::IndexExtension::Zonemap, path))) {
        auto zone = decode_zone(bytes);
        std::uint64_t lo = 0;
        std::uint64_t hi = 0;
        if (zone && parse(zone->min, lo) && parse(zone->max, hi))
            out.emplace(granule, std::make_pair(lo, hi));
    }
    return out;
}

}  // namespace

ankerl::unordered_dense::map<std::uint64_t,
                             std::pair<std::uint64_t, std::uint64_t>>
chunk_time_bounds(const index::Snapshot& snapshot, const index::FileRef& file) {
    return chunk_bounds(snapshot, file, "te");
}

ankerl::unordered_dense::map<std::uint64_t,
                             std::pair<std::uint64_t, std::uint64_t>>
chunk_start_bounds(const index::Snapshot& snapshot,
                   const index::FileRef& file) {
    return chunk_bounds(snapshot, file, "ts");
}

ankerl::unordered_dense::set<std::uint64_t> chunks_without_starts(
    const index::Snapshot& snapshot, const index::FileRef& file,
    std::uint64_t begin, std::uint64_t end) {
    ankerl::unordered_dense::set<std::uint64_t> out;
    if (!index::extension_current(snapshot, file,
                                  index::store::IndexExtension::Zonemap))
        return out;
    for (const auto& [granule, bytes] : common::unwrap(index::granules(
             snapshot, file, index::store::IndexExtension::Zonemap, "ts"))) {
        auto zone = decode_zone(bytes);
        if (zone && zone->histogram && !zone->histogram->empty() &&
            zone->histogram->count_in_range(begin, end) == 0)
            out.insert(granule);
    }
    return out;
}

index::gzip::TimeBounds file_time_bounds(const index::Snapshot& snapshot,
                                         const index::FileRef& file) {
    index::gzip::TimeBounds bounds;
    for (const auto& [granule, range] : chunk_time_bounds(snapshot, file)) {
        if (range.first > range.second) continue;
        bounds.valid = true;
        bounds.min_timestamp_us =
            std::min(bounds.min_timestamp_us, range.first);
        bounds.max_timestamp_us =
            std::max(bounds.max_timestamp_us, range.second);
    }
    return bounds;
}

std::optional<std::pair<double, double>> file_value_range(
    const index::Snapshot& snapshot, const index::FileRef& file,
    std::string_view path) {
    std::optional<std::pair<double, double>> out;
    if (!index::extension_current(snapshot, file,
                                  index::store::IndexExtension::Zonemap))
        return out;
    auto number = [](const std::string& text, double& v) {
        char* end = nullptr;
        v = std::strtod(text.c_str(), &end);
        return !text.empty() && end == text.c_str() + text.size();
    };
    for (const auto& [granule, bytes] : common::unwrap(index::granules(
             snapshot, file, index::store::IndexExtension::Zonemap, path))) {
        auto zone = decode_zone(bytes);
        double lo = 0;
        double hi = 0;
        if (!zone || zone->value_type == "string" || !number(zone->min, lo) ||
            !number(zone->max, hi) || lo > hi)
            continue;
        out = out ? std::make_pair(std::min(out->first, lo),
                                   std::max(out->second, hi))
                  : std::make_pair(lo, hi);
    }
    return out;
}

}  // namespace dftracer::utils::index::extensions::kinds
