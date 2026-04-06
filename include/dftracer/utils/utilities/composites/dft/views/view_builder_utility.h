#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VIEWS_VIEW_BUILDER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VIEWS_VIEW_BUILDER_UTILITY_H

#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter_cache.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::views {

struct ViewBuilderInput {
    ViewDefinition view;
    std::string file_path;
    std::string index_path;  // `.dftindex` store path
    std::size_t uncompressed_size = 0;
    std::size_t num_checkpoints = 0;
    indexing::BloomFilterCache* bloom_cache = nullptr;
    std::optional<std::pair<double, double>> time_range;  // {begin, end}

    // Fluent builders
    ViewBuilderInput& with_view(const ViewDefinition& v);
    ViewBuilderInput& with_file_path(const std::string& path);
    ViewBuilderInput& with_index_path(const std::string& path);
    ViewBuilderInput& with_uncompressed_size(std::size_t s);
    ViewBuilderInput& with_num_checkpoints(std::size_t n);
    ViewBuilderInput& with_bloom_cache(indexing::BloomFilterCache* c);
    ViewBuilderInput& with_time_range(double begin, double end);
};

struct ViewChunkCandidate {
    std::uint64_t checkpoint_idx = 0;
    std::size_t start_byte = 0;
    std::size_t end_byte = 0;
};

struct ViewBuilderOutput {
    bool file_may_match = false;
    std::vector<ViewChunkCandidate> candidates;
    std::uint64_t total_checkpoints = 0;
    std::uint64_t skipped_checkpoints = 0;
    bool success = false;
};

class ViewBuilderUtility : public Utility<ViewBuilderInput, ViewBuilderOutput,
                                          tags::Parallelizable> {
   public:
    coro::CoroTask<ViewBuilderOutput> process(
        const ViewBuilderInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::views

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VIEWS_VIEW_BUILDER_UTILITY_H
