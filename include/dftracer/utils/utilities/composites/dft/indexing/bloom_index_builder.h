#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_INDEX_BUILDER_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_INDEX_BUILDER_H

#include <dftracer/utils/core/utilities/tags/needs_context.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_indexer_utility.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>

#include <cstddef>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::indexing {

inline std::vector<std::string> default_bloom_dimensions() {
    return {"name", "cat", "pid", "tid", "hhash", "fhash", "shash"};
}

struct BloomIndexBuildInput {
    std::string file_path;
    std::string index_dir;
    std::size_t checkpoint_size =
        indexer::internal::Indexer::DEFAULT_CHECKPOINT_SIZE;
    ChunkIndexerConfig indexer_config;
    std::size_t batch_size = 4 * 1024 * 1024;
    bool force_rebuild = false;
    std::vector<std::string> dimensions = default_bloom_dimensions();
};

struct BloomIndexBuildOutput {
    std::string file_path;
    std::string bidx_path;
    bool success = false;
    bool was_skipped = false;
    std::size_t events_processed = 0;
    std::size_t chunks_processed = 0;
    std::string error_message;
};

class BloomIndexBuilderUtility
    : public utilities::Utility<BloomIndexBuildInput, BloomIndexBuildOutput,
                                utilities::tags::NeedsContext> {
   public:
    BloomIndexBuilderUtility() = default;

    coro::CoroTask<BloomIndexBuildOutput> process(
        const BloomIndexBuildInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_BLOOM_INDEX_BUILDER_H
