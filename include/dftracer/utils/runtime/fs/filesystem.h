// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_FS_FILESYSTEM_H
#define DFTRACER_UTILS_RUNTIME_FS_FILESYSTEM_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::runtime::fs {

/// One name in a directory, with the kind the listing reported, or an lstat of
/// the name when it reported none.
struct Entry {
    dstd::Path path;
    dstd::fs::FileKind kind = dstd::fs::FileKind::Unknown;
};

/// An open file of a @ref FileSystem. It can serve concurrent read_at and
/// write_at calls. Destroying it without @ref close closes it and drops any
/// close error.
class FileSystemFile {
   public:
    virtual ~FileSystemFile() = default;

    /// Reads up to `out.size()` bytes at @p offset. Fewer is not an error, and
    /// 0 means @p offset is at or past the end. @p out must stay valid until
    /// the task ends.
    virtual coro::CoroTask<dstd::Expected<std::size_t, dstd::Error>> read_at(
        std::uint64_t offset, dstd::Span<std::uint8_t> out) = 0;

    /// Writes up to `data.size()` bytes at @p offset and gives how many. @p
    /// data must stay valid until the task ends.
    virtual coro::CoroTask<dstd::Expected<std::size_t, dstd::Error>> write_at(
        std::uint64_t offset, dstd::ByteView data) = 0;

    /// Call it once at most, with no read or write running.
    virtual coro::CoroTask<dstd::Expected<void, dstd::Error>> close() = 0;
};

/**
 * @brief Where bytes live: local disk, or any store a user adds with
 * @ref register_file_system.
 *
 * Calls run from any worker and must not block it. A path argument must stay
 * valid until the task ends. Errors use dstd::Condition::NotFound for a path
 * that does not exist, so @ref find can leave out a file that vanishes.
 */
class FileSystem {
   public:
    virtual ~FileSystem() = default;

    virtual coro::CoroTask<
        dstd::Expected<dstd::UniquePtr<FileSystemFile>, dstd::Error>>
    open(dstd::PathView path, dstd::OpenOptions options) = 0;

    /// Follows symlinks.
    virtual coro::CoroTask<dstd::Expected<dstd::fs::FileStat, dstd::Error>>
    stat(dstd::PathView path) = 0;

    /// The names in one directory, in no set order.
    virtual coro::CoroTask<dstd::Expected<dstd::Vector<Entry>, dstd::Error>>
    list(dstd::PathView path) = 0;

    /// Fills `out[i]` for `paths[i]`. Both must stay valid until the task ends
    /// and have one size. The base calls @ref stat for each path in turn.
    /// Override it when a batch costs less, as it does for a thread that makes
    /// many calls in one go.
    virtual coro::CoroTask<void> stat_batch(
        dstd::Span<const dstd::PathView> paths,
        dstd::Span<dstd::Expected<dstd::fs::FileStat, dstd::Error>> out);
};

/// Serves paths that start with `<scheme>://`. A second call for one scheme
/// replaces the first. The scheme "file" stays local. Thread safe. Tasks that
/// run keep the file system they started with.
void register_file_system(dstd::StringView scheme,
                          dstd::SharedPtr<FileSystem> file_system);

/// Stops serving @p scheme. Thread safe.
void unregister_file_system(dstd::StringView scheme);

/// The file system of @p path. A path with no scheme and one with file:// are
/// local. An unknown scheme is an InvalidArgument error. Thread safe.
dstd::Expected<dstd::SharedPtr<FileSystem>, dstd::Error> file_system_for(
    dstd::PathView path);

}  // namespace dftracer::utils::runtime::fs

#endif  // DFTRACER_UTILS_RUNTIME_FS_FILESYSTEM_H
