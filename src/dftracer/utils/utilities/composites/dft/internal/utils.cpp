#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>

#include <functional>
#include <sstream>

namespace dftracer::utils::utilities::composites::dft::internal {

std::string determine_index_path(const std::string& file_path,
                                 const std::string& index_dir) {
    fs::path data_path(file_path);
    std::string base_name =
        data_path.filename().string() + constants::indexer::EXTENSION;

    if (!index_dir.empty()) {
        return (fs::path(index_dir) / base_name).string();
    }

    // Default: place in /tmp/dft_<hash>/ using a hash of the absolute
    // path so that files with the same basename in different directories
    // never collide on the same sidecar.
    std::string abs_path = fs::absolute(data_path).string();
    auto path_hash = std::hash<std::string>{}(abs_path);

    std::ostringstream oss;
    oss << "dft_" << std::hex << path_hash;
    fs::path idx_dir = fs::temp_directory_path() / oss.str();
    fs::create_directories(idx_dir);

    return (idx_dir / base_name).string();
}

}  // namespace dftracer::utils::utilities::composites::dft::internal
