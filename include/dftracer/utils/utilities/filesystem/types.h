#ifndef DFTRACER_UTILS_UTILITIES_FILESYSTEM_TYPES_H
#define DFTRACER_UTILS_UTILITIES_FILESYSTEM_TYPES_H

#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/common/input_stat.h>

#include <cstdint>

namespace dftracer::utils::utilities::filesystem {

/**
 * @brief Output structure representing a file entry.
 */
struct FileEntry {
    fs::path path;
    std::size_t size = 0;
    /// Unix seconds; piggybacks on the size stat so staleness checks need no
    /// extra metadata op. 0 when not populated.
    std::uint64_t mtime = 0;
    bool is_directory = false;
    bool is_regular_file = false;

    FileEntry() = default;

    /// One stat, however many fields are filled.
    explicit FileEntry(const fs::path& p, bool populate_size = true)
        : path(p), size(0), is_directory(false), is_regular_file(false) {
        dftracer::utils::InputFileStat st;
        if (!dftracer::utils::stat_input(p, st)) return;
        is_directory = st.is_directory;
        is_regular_file = st.is_regular_file;
        if (populate_size && is_regular_file) {
            size = static_cast<std::size_t>(st.size);
            mtime = static_cast<std::uint64_t>(st.mtime);
        }
    }

    /// The directory entry's cached type decides directories and, without
    /// sizes, regular files. Anything else costs one stat.
    explicit FileEntry(const fs::directory_entry& entry,
                       bool populate_size = true)
        : path(entry.path()),
          size(0),
          is_directory(false),
          is_regular_file(false) {
        std::error_code ec;
        if (entry.is_directory(ec)) {
            is_directory = true;
            return;
        }
        if (!populate_size && entry.is_regular_file(ec)) {
            is_regular_file = true;
            return;
        }
        dftracer::utils::InputFileStat st;
        if (!dftracer::utils::stat_input(path, st)) return;
        is_directory = st.is_directory;
        is_regular_file = st.is_regular_file;
        if (populate_size && is_regular_file) {
            size = static_cast<std::size_t>(st.size);
            mtime = static_cast<std::uint64_t>(st.mtime);
        }
    }
};

}  // namespace dftracer::utils::utilities::filesystem

#endif  // DFTRACER_UTILS_UTILITIES_FILESYSTEM_TYPES_H
