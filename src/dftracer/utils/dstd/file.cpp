// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/os.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <limits>

namespace dftracer::utils::dstd {

namespace {

constexpr std::uint64_t MAX_OFFSET =
    static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());

Expected<int, Error> open_flags(OpenOptions options) {
    int flags = O_CLOEXEC;
    switch (options.access) {
        case Access::Read:
            flags |= O_RDONLY;
            break;
        case Access::Write:
            flags |= O_WRONLY;
            break;
        case Access::ReadWrite:
            flags |= O_RDWR;
            break;
    }
    if (options.creation != Creation::OpenExisting &&
        options.access == Access::Read) {
        return Unexpected(Error(Condition::InvalidArgument,
                                "open: creating or truncating needs write"));
    }
    switch (options.creation) {
        case Creation::OpenExisting:
            break;
        case Creation::IfNeeded:
            flags |= O_CREAT;
            break;
        case Creation::OnlyIfNew:
            flags |= O_CREAT | O_EXCL;
            break;
        case Creation::TruncateExisting:
            flags |= O_TRUNC;
            break;
        case Creation::AlwaysNew:
            flags |= O_CREAT | O_TRUNC;
            break;
    }
    return flags;
}

Expected<off_t, Error> to_offset(std::uint64_t offset, std::size_t len,
                                 const char* what) {
    if (offset > MAX_OFFSET || len > MAX_OFFSET - offset) {
        return Unexpected(Error(
            Condition::InvalidArgument,
            internal::make_message(what, {}, "offset past the largest file")));
    }
    return static_cast<off_t>(offset);
}

}  // namespace

File::~File() {
    if (handle_ >= 0) ::close(handle_);
}

File::File(File&& other) noexcept : handle_(other.handle_) {
    other.handle_ = -1;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        if (handle_ >= 0) ::close(handle_);
        handle_ = other.handle_;
        other.handle_ = -1;
    }
    return *this;
}

Expected<File, Error> File::open(PathView path, OpenOptions options) {
    auto flags = open_flags(options);
    if (!flags) return Unexpected(flags.error());
    const internal::PathCString c_path(path.str());
    int fd = -1;
    do {
        fd = ::open(c_path.c_str(), *flags, static_cast<mode_t>(options.mode));
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return Unexpected(internal::errno_error(errno, path.str()));
    return File(fd);
}

Expected<void, Error> File::close() noexcept {
    if (handle_ < 0) return {};
    const int fd = handle_;
    handle_ = -1;
    if (::close(fd) != 0 && errno != EINTR) {
        return Unexpected(internal::errno_error(errno, "close"));
    }
    return {};
}

Expected<std::size_t, Error> File::read_at(std::uint64_t offset,
                                           Span<std::uint8_t> out) const {
    auto at = to_offset(offset, out.size(), "pread");
    if (!at) return Unexpected(at.error());
    ssize_t n = -1;
    do {
        n = ::pread(handle_, out.data(), out.size(), *at);
    } while (n < 0 && errno == EINTR);
    if (n < 0) return Unexpected(internal::errno_error(errno, "pread"));
    return static_cast<std::size_t>(n);
}

Expected<std::size_t, Error> File::write_at(std::uint64_t offset,
                                            ByteView data) const {
    auto at = to_offset(offset, data.size(), "pwrite");
    if (!at) return Unexpected(at.error());
    ssize_t n = -1;
    do {
        n = ::pwrite(handle_, data.data(), data.size(), *at);
    } while (n < 0 && errno == EINTR);
    if (n < 0) return Unexpected(internal::errno_error(errno, "pwrite"));
    return static_cast<std::size_t>(n);
}

Expected<void, Error> File::read_exact_at(std::uint64_t offset,
                                          Span<std::uint8_t> out) const {
    std::size_t done = 0;
    while (done < out.size()) {
        auto n = read_at(offset + done, out.subspan(done));
        if (!n) return Unexpected(n.error());
        if (*n == 0) {
            return Unexpected(
                Error(Condition::Io, "pread: end of file before the request"));
        }
        done += *n;
    }
    return {};
}

Expected<void, Error> File::write_all_at(std::uint64_t offset,
                                         ByteView data) const {
    std::size_t done = 0;
    while (done < data.size()) {
        auto n = write_at(offset + done,
                          ByteView(data.data() + done, data.size() - done));
        if (!n) return Unexpected(n.error());
        if (*n == 0) {
            return Unexpected(Error(Condition::Io, "pwrite: wrote 0 bytes"));
        }
        done += *n;
    }
    return {};
}

Expected<std::uint64_t, Error> File::size() const {
    struct stat st{};
    if (::fstat(handle_, &st) != 0) {
        return Unexpected(internal::errno_error(errno, "fstat"));
    }
    return static_cast<std::uint64_t>(st.st_size);
}

Expected<void, Error> File::resize(std::uint64_t size) const {
    auto length = to_offset(size, 0, "ftruncate");
    if (!length) return Unexpected(length.error());
    int rc = -1;
    do {
        rc = ::ftruncate(handle_, *length);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) return Unexpected(internal::errno_error(errno, "ftruncate"));
    return {};
}

Expected<void, Error> File::sync(SyncMode mode) const {
    int rc = -1;
    do {
#if defined(__APPLE__)
        rc = ::fsync(handle_);
#else
        rc = mode == SyncMode::Data ? ::fdatasync(handle_) : ::fsync(handle_);
#endif
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        return Unexpected(internal::errno_error(
            errno, mode == SyncMode::Data ? "fdatasync" : "fsync"));
    }
    return {};
}

}  // namespace dftracer::utils::dstd
