#ifndef DFTRACER_UTILS_INDEX_BUILD_RESOLVE_AND_BUILD_H
#define DFTRACER_UTILS_INDEX_BUILD_RESOLVE_AND_BUILD_H

#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/scoped_fd.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/index/build/chunk_indexer.h>
#include <dftracer/utils/index/build/resolver.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::index::build {

struct ResolveAndBuildInput {
    std::string directory;
    std::vector<std::string> files;
    /// See ResolverInput::entries.
    std::vector<utilities::filesystem::FileEntry> entries;
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
    /// indexed by name besides the path budget's; a file whose index lacks one
    /// of them, or was built with another budget, is rebuilt when
    /// require_bloom is set.
    ChunkIndexerConfig bloom_config;
    bool require_aggregation = false;

    std::optional<index::schemas::dft::agg::AggregationConfig>
        aggregation_config;

    /// See IndexBuildBatchConfig::memory_budget.
    std::uint64_t memory_budget = 0;
    /// See IndexBuildBatchConfig::schema.
    std::string schema;

    /// Also rebuild these tier extensions for every indexed file, whether or
    /// not they are current.
    index::store::ExtensionMask rebuild_extensions;
};

/// Consolidates the common resolve -> build -> re-resolve pattern.
/// Returns ResolverResult with:
///   - all_files: discovered files
///   - index_path: path to shared index
///   - cached: fully resolved files ready for use
///   - needs_checkpoint: files that failed to index (for direct scan fallback)
coro::CoroTask<ResolverResult> resolve_and_build_index(
    CoroScope* scope, ResolveAndBuildInput input);

/// The files of `input` that need a whole-file build: not indexed, changed
/// since they were, or indexed with other settings than `input` names. Uses the
/// same decision as resolve_and_build_index, with the size and mtime of
/// `input.entries` when it holds them. An index in another format is removed
/// first. Each entry holds the path, size and mtime.
coro::CoroTask<std::vector<utilities::filesystem::FileEntry>>
files_needing_index(ResolverInput input);

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

}  // namespace dftracer::utils::index::build

#endif
