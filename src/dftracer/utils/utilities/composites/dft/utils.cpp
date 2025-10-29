#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/utils.h>

namespace dftracer::utils::utilities::composites::dft {

std::string determine_index_path(const std::string& file_path,
                                 const std::string& index_dir) {
    fs::path idx_dir =
        index_dir.empty() ? fs::temp_directory_path() : fs::path(index_dir);
    std::string base_name = fs::path(file_path).filename().string();
    return (idx_dir / (base_name + constants::indexer::EXTENSION)).string();
}

}  // namespace dftracer::utils::utilities::composites::dft
