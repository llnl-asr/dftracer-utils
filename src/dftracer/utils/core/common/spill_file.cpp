#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/spill_dir.h>
#include <dftracer/utils/core/common/spill_file.h>
#include <dftracer/utils/core/common/str_format.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace dftracer::utils {

Result<std::unique_ptr<SpillFile>> SpillFile::create() {
    auto dir = spill_dir();
    if (!dir) return unexpected(dir.error());
    std::string path = (fs::path(*dir) / "dftu_spill_XXXXXX").string();
    std::unique_ptr<SpillFile> f(new SpillFile());
    f->fd_ = ScopedFd(::mkostemp(path.data(), O_CLOEXEC));
    if (f->fd_.get() < 0)
        return make_error(
            ErrorCode::IO,
            str_cat("spill: cannot create a spill file in '", *dir, "' (",
                    std::strerror(errno), "); set ", constants::SPILL_DIR_ENV,
                    " to a writable directory"));
    ::unlink(path.c_str());
    return f;
}

std::uint64_t SpillFile::reserve(std::size_t bytes, std::size_t align) {
    std::uint64_t cur = end_.load(std::memory_order_relaxed);
    for (;;) {
        const std::uint64_t at =
            align > 1 ? (cur + align - 1) / align * align : cur;
        if (end_.compare_exchange_weak(cur, at + bytes,
                                       std::memory_order_relaxed))
            return at;
    }
}

Result<void> SpillFile::write_at(std::uint64_t offset, const void* data,
                                 std::size_t n) {
    const char* p = static_cast<const char*>(data);
    std::size_t done = 0;
    while (done < n) {
        const ssize_t w = ::pwrite(fd_.get(), p + done, n - done,
                                   static_cast<off_t>(offset + done));
        if (w < 0) {
            if (errno == EINTR) continue;
            return make_error(
                ErrorCode::IO,
                str_cat("spill: write failed (", std::strerror(errno),
                        "); the spill directory may be full"));
        }
        if (w == 0)
            return make_error(ErrorCode::IO, "spill: write made no progress");
        done += static_cast<std::size_t>(w);
    }
    return {};
}

Result<std::uint64_t> SpillFile::append(const void* data, std::size_t n) {
    const std::uint64_t at = reserve(n);
    auto written = write_at(at, data, n);
    if (!written) return unexpected(written.error());
    return at;
}

Result<void> SpillFile::read(std::uint64_t offset, void* out,
                             std::size_t n) const {
    char* p = static_cast<char*>(out);
    std::size_t done = 0;
    while (done < n) {
        const ssize_t r = ::pread(fd_.get(), p + done, n - done,
                                  static_cast<off_t>(offset + done));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0)
            return make_error(
                ErrorCode::IO,
                str_cat("spill: short read at offset ", offset + done, " (",
                        r < 0 ? std::strerror(errno) : "end of file", ")"));
        done += static_cast<std::size_t>(r);
    }
    return {};
}

}  // namespace dftracer::utils
