// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_FS_FS_H
#define DFTRACER_UTILS_RUNTIME_FS_FS_H

#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/fs/filesystem.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::runtime::fs {

inline constexpr std::uint64_t NANOS_PER_SECOND = 1000000000;

struct FoundFile {
    dstd::Path path;
    std::uint64_t size = 0;
    std::uint64_t mtime_ns = 0;

    /// The mtime in whole seconds, which the index stores.
    std::uint64_t mtime_seconds() const noexcept {
        return mtime_ns / NANOS_PER_SECOND;
    }
};

struct WalkOptions {
    /// Directories whose name matches one are left out and not entered, in
    /// the syntax of @ref dstd::fs::match_name.
    dstd::Vector<dstd::String> skip_dirs;
    bool recursive = true;
    /// false: the first directory that cannot be read ends the stream with its
    /// error. true: it is left out.
    bool skip_unreadable = false;
};

struct FindOptions {
    /// A file is found when its name matches one, in the syntax of
    /// @ref dstd::fs::match_name. Empty finds every file.
    dstd::Vector<dstd::String> patterns;
    dstd::Vector<dstd::String> skip_dirs;
    bool recursive = true;
    /// false: size and mtime_ns stay 0, except for a symlink, which needs a
    /// stat to tell a file from a directory.
    bool with_stat = true;
    /// Stat calls in flight at most. 0 uses the concurrency of the executor.
    std::size_t limit = 0;
    bool skip_unreadable = false;
};

/// Same calls as @ref dstd::fs, on the @ref FileSystem of the path and off the
/// worker. @p path is copied.
coro::CoroTask<dstd::Expected<dstd::Vector<Entry>, dstd::Error>> list_dir(
    dstd::PathView path);
coro::CoroTask<dstd::Expected<dstd::fs::FileStat, dstd::Error>> stat(
    dstd::PathView path);
coro::CoroTask<dstd::Expected<dstd::String, dstd::Error>> read_file(
    dstd::PathView path);
/// @p data must stay valid until the task ends.
coro::CoroTask<dstd::Expected<void, dstd::Error>> write_file(
    dstd::PathView path, dstd::StringView data);

/// Local paths only. Same results as @ref dstd::fs::exists,
/// @ref dstd::fs::create_directories and @ref dstd::fs::remove, off the worker.
/// @p path is copied.
coro::CoroTask<dstd::Expected<bool, dstd::Error>> exists(dstd::PathView path);
coro::CoroTask<dstd::Expected<bool, dstd::Error>> create_directories(
    dstd::PathView path);
coro::CoroTask<dstd::Expected<bool, dstd::Error>> remove(dstd::PathView path);

/// One result per path, in the order of @p paths, which must stay valid until
/// the task ends and name one file system. Follows symlinks. @p limit is the
/// calls in flight at most, 0 uses the concurrency of the executor.
coro::CoroTask<dstd::Vector<dstd::Expected<dstd::fs::FileStat, dstd::Error>>>
stat_many(dstd::Span<const dstd::Path> paths, std::size_t limit = 0);

/// Every entry below @p root, in no set order, without a stat, listing
/// directories concurrently. A symlink to a directory is reported and not
/// entered. Work runs as children of @p scope, which must join after the
/// stream is dropped. A missing @p root or one that is not a directory is the
/// one error of the stream. @p root is copied.
coro::AsyncGenerator<dstd::Expected<Entry, dstd::Error>> walk(
    coro::Scope& scope, dstd::PathView root, WalkOptions options = {});

/// The regular files below @p root, in no set order, following a symlink to a
/// file. Same concurrency and rules as @ref walk. Dropping the stream stops the
/// work. A file that vanishes during the scan is left out.
coro::AsyncGenerator<dstd::Expected<FoundFile, dstd::Error>> find(
    coro::Scope& scope, dstd::PathView root, FindOptions options = {});

/// The results of @ref find, sorted by path.
coro::CoroTask<dstd::Expected<dstd::Vector<FoundFile>, dstd::Error>> find_all(
    coro::Scope& scope, dstd::PathView root, FindOptions options = {});

/// @ref find_all on a runtime of its own with @p threads workers and
/// @p io_threads I/O threads, run to the end on the calling thread. For a tool
/// whose main thread runs no executor, so the stat calls fan out over a pool.
dstd::Expected<dstd::Vector<FoundFile>, dstd::Error> find_all_on_pool(
    std::size_t threads, std::size_t io_threads, dstd::PathView root,
    FindOptions options = {});

/// A list of names for @ref FindOptions::patterns or @ref
/// FindOptions::skip_dirs.
template <std::size_t N>
dstd::Vector<dstd::String> names_of(const char* const (&names)[N]) {
    return dstd::Vector<dstd::String>(names, names + N);
}

}  // namespace dftracer::utils::runtime::fs

#endif  // DFTRACER_UTILS_RUNTIME_FS_FS_H
