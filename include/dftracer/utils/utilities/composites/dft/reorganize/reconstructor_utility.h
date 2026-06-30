#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_RECONSTRUCTOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_RECONSTRUCTOR_UTILITY_H

#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/utilities/tags/needs_context.h>
#include <dftracer/utils/core/utilities/utility.h>

#include <cstddef>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::reorganize {

struct ReconstructorInput {
    std::string input_dir;
    std::string output_dir;
    std::size_t checkpoint_size = constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    std::size_t parallelism = 0;
    bool compress = true;

    ReconstructorInput& with_input_dir(std::string dir);
    ReconstructorInput& with_output_dir(std::string dir);
    ReconstructorInput& with_checkpoint_size(std::size_t sz);
    ReconstructorInput& with_parallelism(std::size_t n);
    ReconstructorInput& with_compress(bool c);
};

struct ReconstructedFileInfo {
    std::string original_path;
    std::string output_path;
    std::size_t events_written = 0;
    std::size_t bytes_written = 0;
};

struct ReconstructorResult {
    std::vector<ReconstructedFileInfo> files;
    std::size_t total_events = 0;
    std::size_t total_bytes = 0;
    std::size_t total_segments = 0;
};

class ReconstructorUtility
    : public utilities::Utility<ReconstructorInput, ReconstructorResult,
                                utilities::tags::NeedsContext> {
   public:
    coro::CoroTask<ReconstructorResult> process(
        const ReconstructorInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft::reorganize

#endif
