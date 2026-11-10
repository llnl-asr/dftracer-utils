// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_BUILD_RESOLVE_AND_BUILD_H
#define DFTRACER_UTILS_INDEX_BUILD_RESOLVE_AND_BUILD_H

#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/scoped_fd.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/chunk_indexer.h>
#include <dftracer/utils/index/build/resolver.h>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace dftracer::utils::index::build {

struct ResolveAndBuildInput {
    dstd::String directory;
    dstd::Vector<dstd::String> files;
    dstd::Vector<runtime::fs::FoundFile> entries;
    dstd::String index_dir;

    std::size_t checkpoint_size =
        common::constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    /// Files built at once. 0 takes the threads of the process runtime, which
    /// a tool's --executor-threads sets.
    std::size_t parallelism = 0;
    bool force_rebuild = false;

    bool require_checkpoints = true;
    /// Index only whole gzip members, CRC checked, and recover nothing from a
    /// cut tail, for files that are still being written.
    bool whole_members = false;
    bool require_bloom = false;
    /// Build the bloom/stats/dimension tier. Off for aggregation-only
    /// consumers (dfanalyzer) that never read it.
    bool build_bloom = true;
    /// The bloom tier's settings. Its extra_dimensions are args fields
    /// indexed by name besides the path budget's; a file whose index lacks one
    /// of them, or was built with another budget, is rebuilt when
    /// require_bloom is set.
    ChunkIndexerConfig bloom_config;

    /// See IndexerOptions::aggregation.
    index::schemas::dft::agg::AggregationConfig aggregation_config =
        index::schemas::dft::agg::standard_aggregation_config();

    /// See IndexBuildBatchConfig::memory_budget.
    std::uint64_t memory_budget = 0;
    /// See IndexBuildBatchConfig::schema.
    dstd::String schema;

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
coro::CoroTask<dstd::Result<ResolverResult>> resolve_and_build_index(
    ResolveAndBuildInput input);

/// The files of `input` that need a whole-file build: not indexed, changed
/// since they were, or indexed with other settings than `input` names. Uses the
/// size and mtime of `input.entries` when it holds them. An index in another
/// format is removed first.
struct IndexWork {
    dstd::Vector<runtime::fs::FoundFile> files;
    /// Roots whose aggregation tier was built with other settings. The build
    /// clears each before it aggregates into it.
    dstd::Vector<dstd::String> stale_aggregation_roots;
};
coro::CoroTask<dstd::Result<IndexWork>> files_needing_index(
    ResolverInput input);

/// Rebuilds any index root for `directory`/`files` that is missing or stale, so
/// read-before-use entry points never see a changed source. Named coroutines
/// with by-value params (CP.53) so callers, especially coroutine lambdas, hold
/// no vector on their own frame (GCC 12/13 frame bug).
coro::CoroTask<dstd::Result<void>> ensure_indexes_fresh(
    dstd::String directory, dstd::Vector<dstd::String> files,
    dstd::String index_dir, bool force_rebuild = false);

/// Single directory-or-file convenience; builds the vector inside this frame.
coro::CoroTask<dstd::Result<void>> ensure_index_fresh(
    dstd::String directory, dstd::String file, dstd::String index_dir,
    bool force_rebuild = false);

}  // namespace dftracer::utils::index::build

#endif
