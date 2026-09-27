#ifndef DFTRACER_UTILS_INDEX_PLAN_PRUNE_H
#define DFTRACER_UTILS_INDEX_PLAN_PRUNE_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/store/index_database.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::index::plan {

/// The metadata (`ph="M"`) records a scan returns besides data events.
enum class MetadataUse : std::uint8_t {
    /// None: prune for the data events alone.
    NONE,
    /// Metadata records bypass the query: also read every chunk holding one.
    EVERY,
    /// The query selects metadata records only: read the chunks whose
    /// records may match it; data evidence and the time range do not apply.
    RECORDS,
    /// The query selects data events and metadata records alike: read the
    /// chunks either may match in.
    ALL,
};

struct PruneRequest {
    std::string index_path;
    std::string file_path;
    /// Borrowed; nullptr skips query pruning.
    const duql::Query* query = nullptr;
    /// Borrowed open index at `index_path`; opened per call when null.
    index::store::IndexDatabase* db = nullptr;
    /// {begin, end} in microseconds; 0 leaves that side open.
    std::optional<std::pair<double, double>> time_range;
    /// Chunk count to expand "every chunk" to when a time range applies and
    /// the query pruning did not report one.
    std::uint64_t chunk_count = 0;
    /// The window selects events that start in [begin, end), so chunks are
    /// pruned by their start times; otherwise by [first start, last end]
    /// against [begin, end], for events that overlap the window.
    bool by_start = false;
    MetadataUse metadata = MetadataUse::NONE;
};

struct PruneResult {
    /// False only when the index proves no event of the file matches.
    bool file_may_match = true;
    /// Every chunk is a candidate; `candidates` is then empty.
    bool all_chunks = true;
    /// Chunk (gzip member) indices to read, ascending, when !all_chunks.
    std::vector<std::uint64_t> candidates;
    /// Chunk count reported by the index, 0 when unknown.
    std::uint64_t total_chunks = 0;
};

/// Decides which chunks of one file a query must read: the query's
/// Conditions first, then the time range against per-chunk time bounds, then
/// the chunks `metadata` needs. Never drops a chunk the index cannot prove
/// irrelevant. Fails only when per-chunk records it needs cannot be read.
coro::CoroTask<Result<PruneResult>> prune_file(PruneRequest request);

}  // namespace dftracer::utils::index::plan

#endif  // DFTRACER_UTILS_INDEX_PLAN_PRUNE_H
