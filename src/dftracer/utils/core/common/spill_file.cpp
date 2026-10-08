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
    f->fd_ = ScopedFd(::mkstemp(path.data()));
    if (f->fd_.get() < 0)
        return make_error(
            ErrorCode::IO,
            str_cat("spill: cannot create a spill file in '", *dir, "' (",
                    std::strerror(errno), "); set ", constants::SPILL_DIR_ENV,
                    " to a writable directory"));
    ::unlink(path.c_str());
    return f;
}

Result<std::uint64_t> SpillFile::append(const void* data, std::size_t n) {
    const std::uint64_t at = end_.fetch_add(n, std::memory_order_relaxed);
    const char* p = static_cast<const char*>(data);
    std::size_t done = 0;
    while (done < n) {
        const ssize_t w = ::pwrite(fd_.get(), p + done, n - done,
                                   static_cast<off_t>(at + done));
        if (w < 0) {
            if (errno == EINTR) continue;
            return make_error(
                ErrorCode::IO,
                str_cat("spill: write failed (", std::strerror(errno),
                        "); the spill directory may be full"));
        }
        done += static_cast<std::size_t>(w);
    }
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
        if (r <= 0) return make_error(ErrorCode::IO, "spill: short read");
        done += static_cast<std::size_t>(r);
    }
    return {};
}

}  // namespace dftracer::utils
