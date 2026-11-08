// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/os.h>

#include <cerrno>
#include <cstring>

namespace dftracer::utils::dstd::internal {

namespace {

constexpr std::uint64_t NANOSECONDS_PER_SECOND = 1000000000ULL;

fs::FileKind kind_from_mode(std::uint32_t mode) noexcept {
    if (S_ISREG(mode)) return fs::FileKind::File;
    if (S_ISDIR(mode)) return fs::FileKind::Directory;
    if (S_ISLNK(mode)) return fs::FileKind::Symlink;
    return fs::FileKind::Other;
}

std::uint64_t mtime_nanoseconds(const struct stat& info) noexcept {
#if defined(__APPLE__)
    const auto& time = info.st_mtimespec;
#else
    const auto& time = info.st_mtim;
#endif
    return static_cast<std::uint64_t>(time.tv_sec) * NANOSECONDS_PER_SECOND +
           static_cast<std::uint64_t>(time.tv_nsec);
}

}  // namespace

fs::FileStat make_file_stat(const struct stat& info) noexcept {
    fs::FileStat out;
    out.size = static_cast<std::uint64_t>(info.st_size);
    out.mtime_ns = mtime_nanoseconds(info);
    out.dev = static_cast<std::uint64_t>(info.st_dev);
    out.ino = static_cast<std::uint64_t>(info.st_ino);
    out.mode = static_cast<std::uint32_t>(info.st_mode);
    out.kind = kind_from_mode(out.mode);
    return out;
}

Condition condition_from_errno(int error) noexcept {
    if (error == ENOENT || error == ENOTDIR) return Condition::NotFound;
    if (error == EINVAL || error == ENAMETOOLONG) {
        return Condition::InvalidArgument;
    }
    return Condition::Io;
}

String make_message(StringView first, StringView second, StringView text) {
    String message(first);
    if (!second.empty()) {
        message.append(" -> ");
        message.append(second);
    }
    message.append(": ");
    message.append(text);
    return message;
}

Error errno_error(int error, StringView first, StringView second) {
    return Error(condition_from_errno(error),
                 make_message(first, second, std::strerror(error)));
}

}  // namespace dftracer::utils::dstd::internal
