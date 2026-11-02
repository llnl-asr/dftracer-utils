// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILESYSTEM_TYPES_H
#define DFTRACER_UTILS_UTILITIES_FILESYSTEM_TYPES_H

#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>

#include <cstdint>
#include <utility>

namespace dftracer::utils::utilities::filesystem {

/**
 * @brief Output structure representing a file entry.
 */
struct FileEntry {
    dstd::Path path;
    std::size_t size = 0;
    /// Unix seconds; piggybacks on the size stat so staleness checks need no
    /// extra metadata op. 0 when not populated.
    std::uint64_t mtime = 0;
    bool is_directory = false;
    bool is_regular_file = false;

    FileEntry() = default;

    explicit FileEntry(dstd::StringView p, bool populate_size = true)
        : path(p), size(0), is_directory(false), is_regular_file(false) {
        dftracer::utils::common::InputFileStat st;
        if (!dftracer::utils::common::stat_input(path, st)) return;
        is_directory = st.is_directory;
        is_regular_file = st.is_regular_file;
        if (populate_size && is_regular_file) {
            size = static_cast<std::size_t>(st.size);
            mtime = static_cast<std::uint64_t>(st.mtime);
        }
    }

    /// The listing's type decides directories and, without sizes, regular
    /// files. Anything else costs one stat.
    FileEntry(dstd::Path p, dstd::fs::FileKind kind, bool populate_size)
        : path(std::move(p)),
          size(0),
          is_directory(false),
          is_regular_file(false) {
        if (kind == dstd::fs::FileKind::Directory) {
            is_directory = true;
            return;
        }
        if (!populate_size && kind == dstd::fs::FileKind::File) {
            is_regular_file = true;
            return;
        }
        dftracer::utils::common::InputFileStat st;
        if (!dftracer::utils::common::stat_input(path, st)) return;
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
