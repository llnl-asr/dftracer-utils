#ifndef DFTRACER_UTILS_UTILITIES_FILESYSTEM_DIRECTORY_SCANNER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_FILESYSTEM_DIRECTORY_SCANNER_UTILITY_H

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/utilities/tags/needs_context.h>
#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/filesystem/types.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>

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
    bool populate_size = true;

    explicit DirectoryScannerUtilityInput(fs::path p, bool rec = false,
                                          bool with_size = true)
        : path(std::move(p)), recursive(rec), populate_size(with_size) {}

    // Equality operator for caching/hashing
    bool operator==(const DirectoryScannerUtilityInput& other) const {
        return path == other.path && recursive == other.recursive &&
               populate_size == other.populate_size;
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
    : public utilities::Utility<
          DirectoryScannerUtilityInput, std::vector<FileEntry>,
          utilities::tags::Parallelizable, utilities::tags::NeedsContext> {
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
    coro::CoroTask<std::vector<FileEntry>> process(
        const DirectoryScannerUtilityInput& input) override {
        std::vector<fs::directory_entry> raw_entries;

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
                raw_entries.push_back(entry);
            }
        } else {
            // Non-recursive directory iteration
            for (const auto& entry : fs::directory_iterator(input.path)) {
                raw_entries.push_back(entry);
            }
        }

        if (!this->has_context()) {
            std::vector<FileEntry> entries;
            entries.reserve(raw_entries.size());
            for (const auto& entry : raw_entries) {
                entries.emplace_back(entry, input.populate_size);
            }
            co_return entries;
        }

        CoroScope& ctx = this->context();
        std::vector<coro::SpawnFuture<FileEntry>> tasks;
        tasks.reserve(raw_entries.size());
        for (auto& entry : raw_entries) {
            auto entry_copy = std::move(entry);
            tasks.push_back(
                ctx.spawn([entry_copy = std::move(entry_copy),
                           populate_size = input.populate_size](
                              CoroScope&) mutable -> coro::CoroTask<FileEntry> {
                    co_return FileEntry(entry_copy, populate_size);
                }));
        }
        std::vector<FileEntry> entries =
            co_await coro::when_all(std::move(tasks));

        co_return entries;
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
        ::dftracer::utils::utilities::hash::HasherUtility hasher;
        hasher.update(dir.path.string());
        hasher.update(dir.recursive);
        return hasher.get_hash().value;
    }
};
}  // namespace std

#endif  // DFTRACER_UTILS_UTILITIES_FILESYSTEM_DIRECTORY_SCANNER_UTILITY_H
