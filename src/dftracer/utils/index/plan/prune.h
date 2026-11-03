// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_PLAN_PRUNE_H
#define DFTRACER_UTILS_INDEX_PLAN_PRUNE_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>

#include <cstdint>
#include <utility>

namespace dftracer::utils::index {
class Snapshot;
}

namespace dftracer::utils::index::plan {

/// The metadata (`ph="M"`) records a scan returns besides data events.
enum class MetadataUse : std::uint8_t {
    /// None: prune for the data events alone.
    None,
    /// Metadata records bypass the query: also read every chunk holding one.
    Every,
    /// The query selects metadata records only: read the chunks whose
    /// records may match it; data evidence and the time range do not apply.
    Records,
    /// The query selects data events and metadata records alike: read the
    /// chunks either may match in.
    All,
};

struct PruneRequest {
    dstd::String index_path;
    dstd::String file_path;
    /// Borrowed; nullptr skips query pruning.
    const duql::Query* query = nullptr;
    /// Borrowed snapshot of the index at `index_path`, held by the caller
    /// for the whole query. When null, prune_file takes one for the call.
    const index::Snapshot* snapshot = nullptr;
    /// {begin, end} in microseconds; 0 leaves that side open.
    dstd::Optional<dstd::Pair<double, double>> time_range;
    /// Chunk count to expand "every chunk" to when a time range applies and
    /// the query pruning did not report one.
    std::uint64_t chunk_count = 0;
    /// The window selects events that start in [begin, end), so chunks are
    /// pruned by their start times; otherwise by [first start, last end]
    /// against [begin, end], for events that overlap the window.
    bool by_start = false;
    MetadataUse metadata = MetadataUse::None;
};

struct PruneResult {
    /// False only when the index proves no event of the file matches.
    bool file_may_match = true;
    /// Every chunk is a candidate; `candidates` is then empty.
    bool all_chunks = true;
    /// Chunk (gzip member) indices to read, ascending, when !all_chunks.
    dstd::Vector<std::uint64_t> candidates;
    /// Chunk count reported by the index, 0 when unknown.
    std::uint64_t total_chunks = 0;
};

/// Opens the index at `index_path` and takes one snapshot of it.
common::Result<index::Snapshot> open_snapshot(dstd::StringView index_path);

/// Decides which chunks of one file a query must read: the query's
/// Conditions first, then the time range against per-chunk time bounds, then
/// the chunks `metadata` needs. Never drops a chunk the index cannot prove
/// irrelevant. Fails only when per-chunk records it needs cannot be read.
coro::CoroTask<common::Result<PruneResult>> prune_file(PruneRequest request);

}  // namespace dftracer::utils::index::plan

#endif  // DFTRACER_UTILS_INDEX_PLAN_PRUNE_H
