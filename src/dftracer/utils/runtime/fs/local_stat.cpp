// SPDX-License-Identifier: MIT
#ifdef __linux__
#include <dftracer/utils/dstd/os.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/runtime/fs/local_stat.h>
#include <dftracer/utils/runtime/io/ops.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

namespace dftracer::utils::runtime::fs {

namespace {

dstd::fs::FileKind kind_from_mode(std::uint32_t mode) noexcept {
    if (S_ISREG(mode)) return dstd::fs::FileKind::File;
    if (S_ISDIR(mode)) return dstd::fs::FileKind::Directory;
    if (S_ISLNK(mode)) return dstd::fs::FileKind::Symlink;
    return dstd::fs::FileKind::Other;
}

dstd::fs::FileStat make_file_stat(const io::Statx& info) noexcept {
    dstd::fs::FileStat out;
    out.size = info.size;
    out.mtime_ns =
        static_cast<std::uint64_t>(info.mtime.sec) * NANOS_PER_SECOND +
        info.mtime.nsec;
    out.dev = makedev(info.dev_major, info.dev_minor);
    out.ino = info.ino;
    out.mode = info.mode;
    out.kind = kind_from_mode(out.mode);
    return out;
}

}  // namespace

coro::CoroTask<dstd::Expected<dstd::fs::FileStat, dstd::Error>> local_stat(
    dstd::PathView path, bool follow_symlinks) {
    const dstd::internal::PathCString name(path.str());
    io::Statx info;
    io::IoAwaitable op = io::statx(AT_FDCWD, name.c_str(),
                                   follow_symlinks ? 0 : AT_SYMLINK_NOFOLLOW,
                                   io::STATX_BASIC_MASK, &info);
    const ssize_t found = co_await op;
    if (found < 0) {
        co_return dstd::Unexpected(
            dstd::internal::errno_error(static_cast<int>(-found), path.str()));
    }
    co_return make_file_stat(info);
}

}  // namespace dftracer::utils::runtime::fs

#endif  // __linux__
