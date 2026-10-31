#ifndef DFTRACER_UTILS_INDEX_EXTENSIONS_KINDS_TIME_BOUNDS_H
#define DFTRACER_UTILS_INDEX_EXTENSIONS_KINDS_TIME_BOUNDS_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/index/gzip/member.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace dftracer::utils::index {
class Snapshot;
class FileRef;
}  // namespace dftracer::utils::index

namespace dftracer::utils::index::extensions::kinds {

/// Per chunk, [first event start, last event end] from the `te` (event end)
/// zonemap; chunks without usable bounds are absent, and so is every chunk
/// when the zonemap is not current.
ankerl::unordered_dense::map<std::uint64_t,
                             std::pair<std::uint64_t, std::uint64_t>>
chunk_time_bounds(const index::Snapshot& snapshot, const index::FileRef& file);

/// Per chunk, [first event start, last event start] from the `ts` zonemap,
/// under the same conditions as chunk_time_bounds.
ankerl::unordered_dense::map<std::uint64_t,
                             std::pair<std::uint64_t, std::uint64_t>>
chunk_start_bounds(const index::Snapshot& snapshot, const index::FileRef& file);

/// Chunks whose `ts` histogram records no event start in [begin, end), under
/// the same conditions as chunk_time_bounds. A chunk whose start bounds span
/// the window can still hold no start in it (events at ts 0 and later ones).
ankerl::unordered_dense::set<std::uint64_t> chunks_without_starts(
    const index::Snapshot& snapshot, const index::FileRef& file,
    std::uint64_t begin, std::uint64_t end);

/// The union of chunk_time_bounds; `valid` is false when there is none.
index::gzip::TimeBounds file_time_bounds(const index::Snapshot& snapshot,
                                         const index::FileRef& file);

/// [min, max] over the file of the numeric zonemap of `path`, in the path's
/// own unit; nullopt when the zonemap is not current or holds no numeric
/// bounds for the path.
std::optional<std::pair<double, double>> file_value_range(
    const index::Snapshot& snapshot, const index::FileRef& file,
    std::string_view path);

}  // namespace dftracer::utils::index::extensions::kinds

#endif  // DFTRACER_UTILS_INDEX_EXTENSIONS_KINDS_TIME_BOUNDS_H
