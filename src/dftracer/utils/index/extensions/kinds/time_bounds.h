// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_EXTENSIONS_KINDS_TIME_BOUNDS_H
#define DFTRACER_UTILS_INDEX_EXTENSIONS_KINDS_TIME_BOUNDS_H

#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/gzip/member.h>

#include <cstdint>
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
dstd::Result<
    dstd::HashMap<std::uint64_t, dstd::Pair<std::uint64_t, std::uint64_t>>>
chunk_time_bounds(const index::Snapshot& snapshot, const index::FileRef& file);

/// Per chunk, [first event start, last event start] from the `ts` zonemap,
/// under the same conditions as chunk_time_bounds.
dstd::Result<
    dstd::HashMap<std::uint64_t, dstd::Pair<std::uint64_t, std::uint64_t>>>
chunk_start_bounds(const index::Snapshot& snapshot, const index::FileRef& file);

/// Chunks whose `ts` histogram records no event start in [begin, end), under
/// the same conditions as chunk_time_bounds. A chunk whose start bounds span
/// the window can still hold no start in it (events at ts 0 and later ones).
dstd::Result<dstd::HashSet<std::uint64_t>> chunks_without_starts(
    const index::Snapshot& snapshot, const index::FileRef& file,
    std::uint64_t begin, std::uint64_t end);

/// The union of chunk_time_bounds; `valid` is false when there is none.
dstd::Result<index::gzip::TimeBounds> file_time_bounds(
    const index::Snapshot& snapshot, const index::FileRef& file);

/// Per chunk, [min, max] of the numeric zonemap of `path`, in the path's own
/// unit; chunks without numeric bounds are absent, and so is every chunk when
/// the zonemap is not current. Every function here returns the error of a
/// failed index read.
dstd::Result<dstd::HashMap<std::uint64_t, dstd::Pair<double, double>>>
chunk_value_ranges(const index::Snapshot& snapshot, const index::FileRef& file,
                   dstd::StringView path);

/// [min, max] over the file of the numeric zonemap of `path`, in the path's
/// own unit; nullopt when the zonemap is not current or holds no numeric
/// bounds for the path.
dstd::Result<dstd::Optional<dstd::Pair<double, double>>> file_value_range(
    const index::Snapshot& snapshot, const index::FileRef& file,
    dstd::StringView path);

}  // namespace dftracer::utils::index::extensions::kinds

#endif  // DFTRACER_UTILS_INDEX_EXTENSIONS_KINDS_TIME_BOUNDS_H
