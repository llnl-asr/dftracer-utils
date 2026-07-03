#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_MANIFEST_EXTRACTOR_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_MANIFEST_EXTRACTOR_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/organize_visitor.h>
#include <dftracer/utils/utilities/composites/dft/reorganize/reorganization_planner.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::reorganize {

struct ManifestExtractorConfig {
    std::string file_path;
    std::string index_path;
    std::size_t source_file_idx = 0;
    std::vector<PredicateGroup> groups;
    std::vector<std::shared_ptr<coro::Channel<std::shared_ptr<LineBatch>>>>
        group_channels;
    std::size_t batch_size = 1024;
};

// Success payload; failures are reported via Result<ManifestExtractorResult>.
struct ManifestExtractorResult {
    std::size_t events_extracted = 0;
    std::size_t events_unmatched = 0;
};

coro::CoroTask<Result<ManifestExtractorResult>> extract_from_manifest(
    ManifestExtractorConfig config);

}  // namespace dftracer::utils::utilities::composites::dft::reorganize

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_REORGANIZE_MANIFEST_EXTRACTOR_H
