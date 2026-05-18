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

    explicit FileEntry(const fs::path& p, bool populate_size = true)
        : path(p), size(0), is_directory(false), is_regular_file(false) {
        if (fs::exists(p)) {
            is_directory = fs::is_directory(p);
            is_regular_file = fs::is_regular_file(p);
            if (populate_size && is_regular_file) {
                size = fs::file_size(p);
            }
        }
    }

    explicit FileEntry(const fs::directory_entry& entry,
                       bool populate_size = true)
        : path(entry.path()),
          size(0),
          is_directory(false),
          is_regular_file(false) {
        is_directory = entry.is_directory();
        is_regular_file = entry.is_regular_file();
        if (populate_size && is_regular_file) {
            size = static_cast<std::size_t>(entry.file_size());
        }
    }
};

}  // namespace dftracer::utils::utilities::filesystem

#endif  // DFTRACER_UTILS_UTILITIES_FILESYSTEM_TYPES_H
