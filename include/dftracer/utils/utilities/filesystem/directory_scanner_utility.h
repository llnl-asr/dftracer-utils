#ifndef DFTRACER_UTILS_UTILITIES_FILESYSTEM_DIRECTORY_SCANNER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_FILESYSTEM_DIRECTORY_SCANNER_UTILITY_H

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/filesystem/types.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>

#include <functional>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::filesystem {

/// Directories that hold generated `.pfw.gz` output, never input traces, and
/// so must be skipped by a recursive scan or it would ingest derived copies as
/// source files (double-counting events). Two kinds:
/// - Index-artifact dirs (`.dftindex`, `.dftindex-cache`, `.dftindex_staging`
///   and older `.dftindex-views`): index data, rollups and materialized views.
/// - `split/`: rechunked/split output written as a sibling by
///   `normalize_members_for_ingest` and by the `dftracer_split` CLI. A
///   recursive scan re-run after a split would otherwise pick up both the
///   original trace and its split copy.
inline bool is_excluded_scan_dir(const fs::path& p) {
    const std::string name = p.filename().string();
    return name.rfind(".dftindex", 0) == 0 || name == "split";
}

struct DirectoryScannerUtilityInput {
    fs::path path;
    bool recursive = false;
    bool populate_size = true;

    DirectoryScannerUtilityInput() = default;

    explicit DirectoryScannerUtilityInput(fs::path p, bool rec = false,
                                          bool with_size = true)
        : path(std::move(p)), recursive(rec), populate_size(with_size) {}

    bool operator==(const DirectoryScannerUtilityInput& other) const {
        return path == other.path && recursive == other.recursive &&
               populate_size == other.populate_size;
    }

    bool operator!=(const DirectoryScannerUtilityInput& other) const {
        return !(*this == other);
    }
};

/**
 * @brief Utility that scans a directory (optionally recursively) and returns
 * metadata about each file/subdirectory found.
 */
class DirectoryScannerUtility {
   public:
    /**
     * @brief Scan directory and return list of file entries.
     *
     * A recursive scan fans out per subdirectory over @p ctx so many
     * directories are read concurrently (fast on parallel filesystems like
     * Lustre).
     *
     * @throws fs::filesystem_error if directory doesn't exist or is
     * inaccessible
     */
    coro::CoroTask<std::vector<FileEntry>> operator()(
        CoroScope& ctx, const DirectoryScannerUtilityInput& input) const {
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
            co_return co_await scan_parallel(ctx, input.path,
                                             input.populate_size);
        }

        std::vector<fs::directory_entry> raw_entries;
        for (const auto& entry : fs::directory_iterator(input.path)) {
            raw_entries.push_back(entry);
        }

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

    /// Scope-less overload: opens its own CoroScope on the current executor.
    coro::CoroTask<std::vector<FileEntry>> operator()(
        const DirectoryScannerUtilityInput& input) const {
        return with_scope(*this, input);
    }

   private:
    /// Recursive parallel scan: read one directory level, spawn a child scan
    /// per subdirectory (so many directories are read concurrently), then
    /// merge. Uses error codes instead of exceptions so an unreadable directory
    /// is skipped rather than aborting the whole scan.
    static coro::CoroTask<std::vector<FileEntry>> scan_parallel(
        CoroScope& ctx, fs::path dir, bool populate_size) {
        std::vector<FileEntry> files;
        std::vector<coro::SpawnFuture<std::vector<FileEntry>>> subdirs;

        std::error_code ec;
        fs::directory_iterator it(dir, ec);
        const fs::directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            const fs::directory_entry& entry = *it;
            std::error_code sec;
            if (entry.is_directory(sec) && !entry.is_symlink(sec)) {
                if (is_excluded_scan_dir(entry.path())) continue;
                files.emplace_back(entry, populate_size);
                subdirs.push_back(ctx.spawn(
                    [p = entry.path(), populate_size](CoroScope& child)
                        -> coro::CoroTask<std::vector<FileEntry>> {
                        co_return co_await scan_parallel(child, p,
                                                         populate_size);
                    }));
            } else {
                files.emplace_back(entry, populate_size);
            }
        }

        if (!subdirs.empty()) {
            auto nested = co_await coro::when_all(std::move(subdirs));
            for (auto& sub : nested) {
                files.insert(files.end(), std::make_move_iterator(sub.begin()),
                             std::make_move_iterator(sub.end()));
            }
        }
        co_return files;
    }
};

}  // namespace dftracer::utils::utilities::filesystem

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
