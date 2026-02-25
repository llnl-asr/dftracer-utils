#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_MANIFEST_INDEX_BUILDER_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_MANIFEST_INDEX_BUILDER_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utilities/tags/needs_context.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_indexer_utility.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>

#include <cstddef>
#include <string>

namespace dftracer::utils::utilities::composites::dft::indexing {

struct ManifestIndexBuildInput {
    std::string file_path;
    std::string index_dir;
    std::size_t checkpoint_size =
        indexer::internal::Indexer::DEFAULT_CHECKPOINT_SIZE;
    std::size_t batch_size = 4 * 1024 * 1024;
    bool force_rebuild = false;
};

struct ManifestIndexBuildOutput {
    std::string file_path;
    std::string midx_path;
    bool success = false;
    bool was_skipped = false;
    std::size_t events_processed = 0;
    std::size_t chunks_processed = 0;
    std::string error_message;
};

class ManifestIndexBuilderUtility
    : public utilities::Utility<ManifestIndexBuildInput,
                                ManifestIndexBuildOutput,
                                utilities::tags::NeedsContext> {
   public:
    ManifestIndexBuilderUtility() = default;

    coro::CoroTask<ManifestIndexBuildOutput> process(
        const ManifestIndexBuildInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_MANIFEST_INDEX_BUILDER_H
