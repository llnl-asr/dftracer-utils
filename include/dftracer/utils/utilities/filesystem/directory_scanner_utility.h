#ifndef DFTRACER_UTILS_UTILITIES_FILESYSTEM_DIRECTORY_SCANNER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_FILESYSTEM_DIRECTORY_SCANNER_UTILITY_H

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/filesystem/types.h>

#include <functional>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::filesystem {

/**
 * @brief Input structure representing a directory to scan.
 */
struct DirectoryScannerUtilityInput {
    fs::path path;
    bool recursive = false;  // Whether to scan subdirectories

    explicit DirectoryScannerUtilityInput(fs::path p, bool rec = false)
        : path(std::move(p)), recursive(rec) {}

    // Equality operator for caching/hashing
    bool operator==(const DirectoryScannerUtilityInput& other) const {
        return path == other.path && recursive == other.recursive;
    }

    bool operator!=(const DirectoryScannerUtilityInput& other) const {
        return !(*this == other);
    }
};

/**
 * @brief Utility that scans a directory and returns a list of file entries.
 *
 * This utility scans a directory (optionally recursively) and returns
 * metadata about each file/subdirectory found.
 *
 * Features:
 * - Non-recursive scanning (default)
 * - Recursive scanning when Directory.recursive = true
 * - Returns file metadata (path, size, type)
 * - Can be composed with other utilities in a pipeline
 *
 * Usage:
 * @code
 * auto scanner = std::make_shared<DirectoryScanner>();
 * auto result = scanner->process(Directory{"/path/to/dir"});
 * for (const auto& entry : result) {
 *     std::cout << entry.path << " - " << entry.size << " bytes\n";
 * }
 * @endcode
 */
class DirectoryScannerUtility
    : public utilities::Utility<DirectoryScannerUtilityInput,
                                std::vector<FileEntry>,
                                utilities::tags::Parallelizable> {
   public:
    DirectoryScannerUtility() = default;
    ~DirectoryScannerUtility() = default;

    /**
     * @brief Scan directory and return list of file entries.
     *
     * @param input Directory to scan (with optional recursive flag)
     * @return Vector of FileEntry objects
     * @throws fs::filesystem_error if directory doesn't exist or is
     * inaccessible
     */
    std::vector<FileEntry> process(
        const DirectoryScannerUtilityInput& input) override {
        std::vector<FileEntry> entries;

        if (!fs::exists(input.path)) {
            throw fs::filesystem_error(
                "Directory does not exist", input.path,
                std::make_error_code(std::errc::no_such_file_or_directory));
        }

        if (!fs::is_directory(input.path)) {
            throw fs::filesystem_error(
                "Path is not a directory", input.path,
                std::make_error_code(std::errc::not_a_directory));
        }

        if (input.recursive) {
            // Recursive directory iteration
            for (const auto& entry :
                 fs::recursive_directory_iterator(input.path)) {
                entries.emplace_back(entry.path());
            }
        } else {
            // Non-recursive directory iteration
            for (const auto& entry : fs::directory_iterator(input.path)) {
                entries.emplace_back(entry.path());
            }
        }

        return entries;
    }
};

}  // namespace dftracer::utils::utilities::filesystem

// Hash specialization for DirectoryScannerUtilityInput to enable caching
namespace std {
template <>
struct hash<
    dftracer::utils::utilities::filesystem::DirectoryScannerUtilityInput> {
    std::size_t operator()(
        const dftracer::utils::utilities::filesystem::
            DirectoryScannerUtilityInput& dir) const noexcept {
        std::size_t h1 = std::hash<std::string>{}(dir.path.string());
        std::size_t h2 = std::hash<bool>{}(dir.recursive);
        return h1 ^ (h2 << 1);
    }
};
}  // namespace std

#endif  // DFTRACER_UTILS_UTILITIES_FILESYSTEM_DIRECTORY_SCANNER_UTILITY_H
