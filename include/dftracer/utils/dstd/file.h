// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_FILE_H
#define DFTRACER_UTILS_DSTD_FILE_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::dstd {

/// The operating system's handle of an open file: a file descriptor here.
using NativeHandle = int;

enum class Access : std::uint8_t { Read, Write, ReadWrite };

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

/// How @ref File::open opens a file. Every Creation other than OpenExisting
/// needs write access. `mode` holds the permission bits of a new file.
struct OpenOptions {
    Access access = Access::Read;
    Creation creation = Creation::OpenExisting;
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
    /// failure for an invalid combination.
    static Expected<int, Error> native_open_flags(OpenOptions options);

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

    Expected<std::uint64_t, Error> size() const;

    /// Sets the size in bytes. Bytes added at the end read as zero.
    Expected<void, Error> resize(std::uint64_t size) const;

    Expected<void, Error> sync(SyncMode mode = SyncMode::Data) const;

    bool is_open() const noexcept { return handle_ >= 0; }

    /// The handle stays owned by this File and is valid until it closes.
    NativeHandle native_handle() const noexcept { return handle_; }

   private:
    explicit File(NativeHandle handle) noexcept : handle_(handle) {}

    NativeHandle handle_ = -1;
};

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_FILE_H
