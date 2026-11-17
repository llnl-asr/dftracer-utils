// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_CACHE_PHASE_SEED_STORE_H
#define DFTRACER_UTILS_TRACE_INDEX_CACHE_PHASE_SEED_STORE_H

#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/cache/cache_store.h>

#include <cstdint>

namespace dftracer::utils::trace::views::detail {
struct ViewPlan;
}  // namespace dftracer::utils::trace::views::detail

namespace dftracer::utils::trace::index::cache {

/// The facts about the input files of a run that a plugin phase can be seeded
/// with, over every non-metadata event of every file.
struct InputStats {
    std::uint64_t event_count = 0;
    /// The earliest event start above 0; 0 when no event has one.
    std::uint64_t ts_min = 0;
    /// The latest event end (start + duration); 0 when there is no event.
    std::uint64_t ts_max = 0;

    bool operator==(const InputStats&) const = default;
};

/// The stats of `plan`'s files from their indexes, merged. Nullopt unless every
/// file has an index that is current for the file's stat and holds its
/// statistics, and for a plan that pins a file, whose view covers less than the
/// index. A failed index read is a miss too, with one warning logged, so the
/// caller falls through to its next source.
dstd::Optional<InputStats> index_input_stats(
    const trace::views::detail::ViewPlan& plan);

/// The identity of each input file of `plan`, by logical path: a hash of its
/// mtime in nanoseconds, size and sampled content hash. In plan order. Nullopt
/// when a file cannot be stat'ed, or for a plan that pins a file.
dstd::Optional<dstd::Vector<dstd::Pair<dstd::String, std::uint64_t>>>
input_file_keys(const trace::views::detail::ViewPlan& plan);

/// The fingerprint of an `input_file_keys` list, as `input_fingerprint` of the
/// plan that gave it. `keys` need not be sorted.
std::uint64_t fingerprint_of_keys(
    dstd::Vector<dstd::Pair<dstd::String, std::uint64_t>> keys);

/// The identity of the inputs: a hash of the index format and api versions and
/// each file's logical path, mtime in nanoseconds, size and sampled content
/// hash. It does not read an index. Nullopt when a file cannot be stat'ed, or
/// for a plan that pins a file.
dstd::Optional<std::uint64_t> input_fingerprint(
    const trace::views::detail::ViewPlan& plan);

/// Where the stats cache of `plan` lives: `<index root>/phase_seeds`. Empty
/// when the plan has no single index root.
dstd::String phase_seed_cache_path(const trace::views::detail::ViewPlan& plan);

/// The stats stored for `fingerprint`, or nullopt when there are none or the
/// entry is of another format. A hit does not refresh the entry's LRU time: the
/// values are tiny, so eviction by write time is enough, and a hit must not
/// write.
dstd::Result<dstd::Optional<InputStats>> read_input_stats(
    CacheStore& db, std::uint64_t fingerprint);

/// Stores `stats` for `fingerprint`, evicting the least recently used entries
/// past cache_max_bytes().
dstd::Result<void> persist_input_stats(CacheStore& db,
                                       std::uint64_t fingerprint,
                                       const InputStats& stats);

}  // namespace dftracer::utils::trace::index::cache

#endif  // DFTRACER_UTILS_TRACE_INDEX_CACHE_PHASE_SEED_STORE_H
