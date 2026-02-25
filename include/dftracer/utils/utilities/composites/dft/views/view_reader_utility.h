#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VIEWS_VIEW_READER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VIEWS_VIEW_READER_UTILITY_H

#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::views {

struct ViewReaderInput {
    std::string file_path;
    std::string idx_path;
    std::size_t checkpoint_size = 0;
    std::size_t start_byte = 0;
    std::size_t end_byte = 0;
    std::uint64_t checkpoint_idx = 0;
    std::size_t batch_size = 4 * 1024 * 1024;  // 4MB
    ViewDefinition view;

    // Fluent builders
    ViewReaderInput& with_file_path(const std::string& path);
    ViewReaderInput& with_idx_path(const std::string& path);
    ViewReaderInput& with_checkpoint_size(std::size_t sz);
    ViewReaderInput& with_byte_range(std::size_t start, std::size_t end);
    ViewReaderInput& with_checkpoint_idx(std::uint64_t idx);
    ViewReaderInput& with_batch_size(std::size_t sz);
    ViewReaderInput& with_view(const ViewDefinition& v);
};

struct ViewReaderOutput {
    std::vector<std::string> events;  // matching JSON lines
    std::uint64_t events_matched = 0;
    std::uint64_t events_scanned = 0;
    bool success = false;
};

class ViewReaderUtility
    : public Utility<ViewReaderInput, ViewReaderOutput, tags::Parallelizable> {
   public:
    coro::CoroTask<ViewReaderOutput> process(
        const ViewReaderInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::views

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VIEWS_VIEW_READER_UTILITY_H