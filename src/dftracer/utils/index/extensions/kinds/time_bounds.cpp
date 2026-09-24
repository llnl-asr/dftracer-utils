#include <dftracer/utils/index/extensions/kinds/payloads.h>
#include <dftracer/utils/index/extensions/kinds/time_bounds.h>

#include <algorithm>
#include <charconv>

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
chunk_bounds(const index::store::IndexDatabase& db, int file_id,
             std::string_view path) {
    ankerl::unordered_dense::map<std::uint64_t,
                                 std::pair<std::uint64_t, std::uint64_t>>
        out;
    if (!db.extension_current(file_id, index::store::IndexExtension::ZONEMAP))
        return out;
    for (const auto& [granule, bytes] :
         db.path_granules(file_id, index::store::IndexExtension::ZONEMAP,
                          std::string(path))) {
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
chunk_time_bounds(const index::store::IndexDatabase& db, int file_id) {
    return chunk_bounds(db, file_id, "te");
}

ankerl::unordered_dense::map<std::uint64_t,
                             std::pair<std::uint64_t, std::uint64_t>>
chunk_start_bounds(const index::store::IndexDatabase& db, int file_id) {
    return chunk_bounds(db, file_id, "ts");
}

ankerl::unordered_dense::set<std::uint64_t> chunks_without_starts(
    const index::store::IndexDatabase& db, int file_id, std::uint64_t begin,
    std::uint64_t end) {
    ankerl::unordered_dense::set<std::uint64_t> out;
    if (!db.extension_current(file_id, index::store::IndexExtension::ZONEMAP))
        return out;
    for (const auto& [granule, bytes] : db.path_granules(
             file_id, index::store::IndexExtension::ZONEMAP, "ts")) {
        auto zone = decode_zone(bytes);
        if (zone && zone->histogram && !zone->histogram->empty() &&
            zone->histogram->count_in_range(begin, end) == 0)
            out.insert(granule);
    }
    return out;
}

index::gzip::TimeBounds file_time_bounds(const index::store::IndexDatabase& db,
                                         int file_id) {
    index::gzip::TimeBounds bounds;
    for (const auto& [granule, range] : chunk_time_bounds(db, file_id)) {
        if (range.first > range.second) continue;
        bounds.valid = true;
        bounds.min_timestamp_us =
            std::min(bounds.min_timestamp_us, range.first);
        bounds.max_timestamp_us =
            std::max(bounds.max_timestamp_us, range.second);
    }
    return bounds;
}

}  // namespace dftracer::utils::index::extensions::kinds
