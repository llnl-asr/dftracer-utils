#ifndef DFTRACER_UTILS_TRACE_INDEXING_RESOLVE_AND_BUILD_H
#define DFTRACER_UTILS_TRACE_INDEXING_RESOLVE_AND_BUILD_H

#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/trace/indexing/chunk_indexer_utility.h>
#include <dftracer/utils/trace/indexing/index_resolver_utility.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::trace::indexing {

struct ResolveAndBuildInput {
    std::string directory;
    std::vector<std::string> files;
    std::string index_dir;

    std::size_t checkpoint_size = constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    std::size_t parallelism = 0;
    bool force_rebuild = false;

    bool require_checkpoints = true;
    bool require_bloom = false;
    /// Build the bloom/stats/dimension tier. Off for aggregation-only
    /// consumers (dfanalyzer) that never read it.
    bool build_bloom = true;
    /// The bloom tier's settings. Its extra_dimensions are args fields
    /// indexed by name besides auto_fields; a file whose index lacks one of
    /// them, or auto_fields, is rebuilt when require_bloom is set.
    ChunkIndexerConfig bloom_config;
    bool require_aggregation = false;

    std::optional<aggregators::AggregationConfig> aggregation_config;
};

/// Consolidates the common resolve -> build -> re-resolve pattern.
/// Returns ResolverResult with:
///   - all_files: discovered files
///   - index_path: path to shared index
///   - cached: fully resolved files ready for use
///   - needs_checkpoint: files that failed to index (for direct scan fallback)
coro::CoroTask<ResolverResult> resolve_and_build_index(
    CoroScope* scope, ResolveAndBuildInput input);

/// Rebuilds any index root for `directory`/`files` that is missing or stale, so
/// read-before-use entry points never see a changed source. Named coroutines
/// with by-value params (CP.53) so callers, especially coroutine lambdas, hold
/// no vector on their own frame (GCC 12/13 frame bug).
coro::CoroTask<void> ensure_indexes_fresh(CoroScope* scope,
                                          std::string directory,
                                          std::vector<std::string> files,
                                          std::string index_dir,
                                          bool force_rebuild = false);

/// Single directory-or-file convenience; builds the vector inside this frame.
coro::CoroTask<void> ensure_index_fresh(CoroScope* scope, std::string directory,
                                        std::string file, std::string index_dir,
                                        bool force_rebuild = false);

/// Rewrite any single/oversized-member input to bounded multi-member gzip in a
/// sibling `split/` dir (non-destructive; the original is untouched), so index
/// and reader never hold a giant member. Returns the file list with those
/// inputs replaced by their split copies, plus the (original, split) pairs for
/// a warning. Call before indexing and read from the returned files so both
/// agree. `member_size` 0 = default checkpoint size.
struct MemberNormalizeResult {
    std::vector<std::string> files;
    std::vector<std::pair<std::string, std::string>> split;
};
coro::CoroTask<MemberNormalizeResult> normalize_members_for_ingest(
    std::vector<std::string> files, std::uint64_t member_size = 0);

}  // namespace dftracer::utils::trace::indexing

#endif
