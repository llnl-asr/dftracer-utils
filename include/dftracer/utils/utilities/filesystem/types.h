#ifndef DFTRACER_UTILS_UTILITIES_FILESYSTEM_TYPES_H
#define DFTRACER_UTILS_UTILITIES_FILESYSTEM_TYPES_H

#include <dftracer/utils/core/common/filesystem.h>

namespace dftracer::utils::utilities::filesystem {

/**
 * @brief Output structure representing a file entry.
 */
struct FileEntry {
    fs::path path;
    std::size_t size = 0;
    bool is_directory = false;
    bool is_regular_file = false;

    FileEntry() = default;

    explicit FileEntry(const fs::path& p)
        : path(p), size(0), is_directory(false), is_regular_file(false) {
        if (fs::exists(p)) {
            is_directory = fs::is_directory(p);
            is_regular_file = fs::is_regular_file(p);
            if (is_regular_file) {
                size = fs::file_size(p);
            }
        }
    }
};

}  // namespace dftracer::utils::utilities::filesystem

#endif  // DFTRACER_UTILS_UTILITIES_FILESYSTEM_TYPES_H
