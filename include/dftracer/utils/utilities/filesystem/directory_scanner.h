#ifndef DFTRACER_UTILS_UTILITIES_FILESYSTEM_DIRECTORY_SCANNER_H
#define DFTRACER_UTILS_UTILITIES_FILESYSTEM_DIRECTORY_SCANNER_H

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utility.h>

#include <functional>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::filesystem {

/**
 * @brief Input structure representing a directory to scan.
 */
struct Directory {
    fs::path path;
    bool recursive = false;  // Whether to scan subdirectories

    explicit Directory(fs::path p, bool rec = false)
        : path(std::move(p)), recursive(rec) {}

    // Equality operator for caching/hashing
    bool operator==(const Directory& other) const {
        return path == other.path && recursive == other.recursive;
    }

    bool operator!=(const Directory& other) const { return !(*this == other); }
};

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
 *
 * With pipeline:
 * @code
 * Pipeline pipeline;
 * auto task = use(scanner).emit_on(pipeline);
 * auto output = SequentialExecutor().execute(pipeline, Directory{"/path"});
 * auto files = output.get<std::vector<FileEntry>>(task.id());
 * @endcode
 */
class DirectoryScannerUtility
    : public utilities::Utility<Directory, std::vector<FileEntry>,
                                utilities::tags::Parallelizable> {
   public:
    DirectoryScannerUtility() = default;
    ~DirectoryScannerUtility() override = default;

    /**
     * @brief Scan directory and return list of file entries.
     *
     * @param input Directory to scan (with optional recursive flag)
     * @return Vector of FileEntry objects
     * @throws fs::filesystem_error if directory doesn't exist or is
     * inaccessible
     */
    std::vector<FileEntry> process(const Directory& input) override {
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

// Hash specialization for Directory to enable caching
namespace std {
template <>
struct hash<dftracer::utils::utilities::filesystem::Directory> {
    std::size_t operator()(
        const dftracer::utils::utilities::filesystem::Directory& dir)
        const noexcept {
        // Combine hash of path and recursive flag
        std::size_t h1 = std::hash<std::string>{}(dir.path.string());
        std::size_t h2 = std::hash<bool>{}(dir.recursive);
        return h1 ^ (h2 << 1);  // Simple hash combination
    }
};
}  // namespace std

#endif  // DFTRACER_UTILS_UTILITIES_FILESYSTEM_DIRECTORY_SCANNER_H
