#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VIEWS_VIEW_BUILDER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VIEWS_VIEW_BUILDER_UTILITY_H

#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::views {

struct ViewBuilderInput {
    ViewDefinition view;
    std::string file_path;
    std::string bidx_path;  // bloom index sidecar path
    std::size_t uncompressed_size = 0;
    std::size_t num_checkpoints = 0;

    // Fluent builders
    ViewBuilderInput& with_view(const ViewDefinition& v);
    ViewBuilderInput& with_file_path(const std::string& path);
    ViewBuilderInput& with_bidx_path(const std::string& path);
    ViewBuilderInput& with_uncompressed_size(std::size_t s);
    ViewBuilderInput& with_num_checkpoints(std::size_t n);
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