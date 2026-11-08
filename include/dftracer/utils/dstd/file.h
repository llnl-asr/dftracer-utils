// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_FILE_H
#define DFTRACER_UTILS_DSTD_FILE_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::dstd {

/// The operating system's handle of an open file: a file descriptor here.
using NativeHandle = int;

/// Append opens for writing at the end only: @ref File::append writes, and the
/// positional writes must not be used on that File.
enum class Access : std::uint8_t { Read, Write, ReadWrite, Append };

/// What @ref File::open does with the file at the path.
enum class Creation : std::uint8_t {
    /// Open it, fail with NotFound when it does not exist.
    OpenExisting,
    /// Open it, create it when it does not exist.
    IfNeeded,
    /// Create it, fail when it exists.
    OnlyIfNew,
    /// Open it and cut it to 0 bytes, fail with NotFound when it does not
    /// exist.
    TruncateExisting,
    /// Create it, or cut it to 0 bytes when it exists.
    AlwaysNew,
};

enum class SyncMode : std::uint8_t {
    /// The data and the metadata needed to read it back (fdatasync).
    Data,
    /// The data and all metadata (fsync).
    All,
};

/// How a File uses the page cache.
enum class Caching : std::uint8_t {
    Normal,
    /// Reads skip the page cache: O_DIRECT on Linux, F_NOCACHE on macOS. Each
    /// offset, length and buffer address must be a multiple of
    /// @ref File::direct_alignment. A file system that refuses it fails the
    /// open with Condition::Unsupported. Only @ref File::open takes it.
    Bypass,
};

/// The access pattern a File announces to the kernel.
enum class Advice : std::uint8_t { Normal, Sequential };

/// How @ref File::open opens a file. Every Creation other than OpenExisting
/// needs write access. `mode` holds the permission bits of a new file.
struct OpenOptions {
    Access access = Access::Read;
    Creation creation = Creation::OpenExisting;
    Caching caching = Caching::Normal;
    std::uint32_t mode = 0644;
};

/**
 * @brief An open file, read and written at explicit offsets only.
 *
 * File has no cursor and no append mode, so one File can serve concurrent
 * positional reads and writes from many threads. open, close and moves are not
 * thread safe. The destructor closes the file and drops a close error; call
 * @ref close to see it.
 */
class File {
   public:
    File() noexcept = default;
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;

    static Expected<File, Error> open(PathView path, OpenOptions options);

    /// Takes ownership of @p handle, which must be an open file descriptor
    /// that nothing else closes. The File closes it.
    static File from_native_handle(NativeHandle handle) noexcept {
        return File(handle);
    }

    /// The open(2) flags @ref open uses for @p options, with the same
    /// failure for an invalid combination. Caching::Bypass needs more than
    /// flags on macOS, so it fails here with Condition::Unsupported there.
    static Expected<int, Error> native_open_flags(OpenOptions options);

    /// The alignment that Caching::Bypass needs for files at @p path, from
    /// statx STATX_DIOALIGN where the kernel reports it, else 4096. 0 when the
    /// file system has no direct I/O.
    static std::uint32_t direct_alignment(PathView path);

    /// Closes the file. A second call does nothing and succeeds.
    Expected<void, Error> close() noexcept;

    /// Reads up to `out.size()` bytes at @p offset. Fewer bytes is not an
    /// error, and 0 means @p offset is at or past the end of the file.
    Expected<std::size_t, Error> read_at(std::uint64_t offset,
                                         Span<std::uint8_t> out) const;

    /// Writes up to `data.size()` bytes at @p offset and returns how many.
    Expected<std::size_t, Error> write_at(std::uint64_t offset,
                                          ByteView data) const;

    /// Fills all of @p out from @p offset. An end of file before that is an
    /// Io error, and the contents of @p out are then unspecified.
    Expected<void, Error> read_exact_at(std::uint64_t offset,
                                        Span<std::uint8_t> out) const;

    /// Writes all of @p data at @p offset or fails.
    Expected<void, Error> write_all_at(std::uint64_t offset,
                                       ByteView data) const;

    /// The buffers one @ref read_vectored_at call fills at most.
    static constexpr std::size_t MAX_VECTORED_BUFFERS = 1024;

    /// Reads into the first @ref MAX_VECTORED_BUFFERS of @p buffers in order
    /// from @p offset with one call and returns the bytes read, which can be
    /// fewer than they hold.
    Expected<std::size_t, Error> read_vectored_at(
        std::uint64_t offset, Span<const Span<std::uint8_t>> buffers) const;

    /// Writes @p data at the end with one write call, so that writers to the
    /// same file do not interleave inside it. Needs Access::Append.
    Expected<std::size_t, Error> append(ByteView data) const;

    Expected<std::uint64_t, Error> size() const;

    Expected<fs::FileStat, Error> stat() const;

    /// The metadata of an open @p handle that this call does not own.
    static Expected<fs::FileStat, Error> stat_of(NativeHandle handle);

    /// A hint for the whole file. Does nothing where the system has no hint.
    Expected<void, Error> advise(Advice advice) const;

    /// Takes the exclusive advisory lock (flock) without waiting. False when
    /// another open file holds it. The lock goes with @ref unlock or close.
    Expected<bool, Error> try_lock() const;

    Expected<void, Error> unlock() const;

    /// Sets the size in bytes. Bytes added at the end read as zero.
    Expected<void, Error> resize(std::uint64_t size) const;

    Expected<void, Error> sync(SyncMode mode = SyncMode::Data) const;

    bool is_open() const noexcept { return handle_ >= 0; }

    /// The handle stays owned by this File and is valid until it closes.
    NativeHandle native_handle() const noexcept { return handle_; }

    /// Gives up the handle without closing it. The caller closes it.
    NativeHandle release() noexcept {
        const NativeHandle handle = handle_;
        handle_ = -1;
        return handle;
    }

   private:
    explicit File(NativeHandle handle) noexcept : handle_(handle) {}

    NativeHandle handle_ = -1;
};

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_FILE_H
