#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_RESOLVE_AND_BUILD_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_RESOLVE_AND_BUILD_H

#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/composites/dft/indexing/index_resolver_utility.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::indexing {

struct ResolveAndBuildInput {
    std::string directory;
    std::vector<std::string> files;
    std::string index_dir;

    std::size_t checkpoint_size = constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    std::size_t parallelism = 0;
    bool force_rebuild = false;

    bool require_checkpoints = true;
    bool require_bloom = false;
    bool require_manifest = false;
    bool require_aggregation = false;

    std::optional<aggregators::AggregationConfig> aggregation_config;
};

// Consolidates the common resolve -> build -> re-resolve pattern.
// Returns ResolverResult with:
//   - all_files: discovered files
//   - index_path: path to shared index
//   - cached: fully resolved files ready for use
//   - needs_checkpoint: files that failed to index (for direct scan fallback)
coro::CoroTask<ResolverResult> resolve_and_build_index(
    CoroScope* scope, ResolveAndBuildInput input);

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif
