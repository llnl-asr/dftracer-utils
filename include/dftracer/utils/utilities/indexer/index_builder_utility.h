#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_BUILDER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_BUILDER_UTILITY_H

#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utilities/tags/needs_context.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_indexer_utility.h>

#include <cstddef>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::indexer {

inline std::vector<std::string> default_bloom_dimensions() {
    return {"name", "cat", "pid", "tid", "hhash", "fhash", "shash"};
}

struct IndexBuildConfig {
    std::string file_path;
    std::string index_dir;
    std::size_t checkpoint_size = 32 * 1024 * 1024;
    std::size_t index_threshold =
        constants::indexer::DEFAULT_INDEX_SIZE_THRESHOLD;
    bool force_rebuild = false;
    bool build_bloom = false;
    bool build_manifest = false;
    composites::dft::indexing::ChunkIndexerConfig bloom_config;
    std::vector<std::string> bloom_dimensions;

    static IndexBuildConfig for_file(const std::string& path);
    IndexBuildConfig& with_index_dir(const std::string& dir);
    IndexBuildConfig& with_checkpoint_size(std::size_t size);
    IndexBuildConfig& with_index_threshold(std::size_t threshold);
    IndexBuildConfig& with_force_rebuild(bool force);
    IndexBuildConfig& with_bloom(bool enable = true);
    IndexBuildConfig& with_manifest(bool enable = true);
    IndexBuildConfig& with_bloom_config(
        const composites::dft::indexing::ChunkIndexerConfig& config);
    IndexBuildConfig& with_bloom_dimensions(std::vector<std::string> dims);
};

struct IndexBuildResult {
    std::string file_path;
    std::string idx_path;
    bool success = false;
    bool was_skipped = false;
    bool index_created = false;
    std::size_t events_processed = 0;
    std::size_t chunks_processed = 0;
    std::size_t total_lines = 0;
    std::string error_message;
};

class IndexBuilderUtility
    : public Utility<IndexBuildConfig, IndexBuildResult, tags::NeedsContext> {
   public:
    coro::CoroTask<IndexBuildResult> process(
        const IndexBuildConfig& config) override;
};

}  // namespace dftracer::utils::utilities::indexer

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INDEX_BUILDER_UTILITY_H
