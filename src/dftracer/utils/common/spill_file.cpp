// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/spill_dir.h>
#include <dftracer/utils/common/spill_file.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <fcntl.h>
#include <stdlib.h>

#include <cerrno>
#include <cstring>
#include <memory>

namespace dftracer::utils::common {

dstd::Result<dstd::UniquePtr<SpillFile>> SpillFile::create() {
    auto dir = spill_dir();
    if (!dir) return dstd::Unexpected(dir.error());
    dstd::String path((dir->view() / "dftu_spill_XXXXXX").str());
    auto f = dstd::make_unique<SpillFile>(Key{}, spill_dir_in_memory(*dir));
    f->file_ =
        dstd::File::from_native_handle(::mkostemp(path.data(), O_CLOEXEC));
    if (!f->file_.is_open())
        return make_error(
            ErrorCode::Io,
            str_cat("spill: cannot create a spill file in '", dir->str(), "' (",
                    std::strerror(errno), "); set ", constants::SPILL_DIR_ENV,
                    " to a writable directory"));
    dstd::fs::remove(dstd::PathView(path)).discard();
    return f;
}

std::uint64_t SpillFile::reserve(std::size_t bytes, std::size_t align) {
    std::uint64_t cur = end_.load(std::memory_order_relaxed);
    for (;;) {
        const std::uint64_t at =
            align > 1 ? (cur + align - 1) / align * align : cur;
        if (end_.compare_exchange_weak(cur, at + bytes,
                                       std::memory_order_relaxed)) {
            charge_.add(at + bytes - cur);
            return at;
        }
    }
}

dstd::Result<void> SpillFile::write_at(std::uint64_t offset, const void* data,
                                       std::size_t n) {
    auto written = file_.write_all_at(
        offset, dstd::ByteView(static_cast<const char*>(data), n));
    if (!written)
        return make_error(
            ErrorCode::Io,
            str_cat("spill: write failed (", written.error().format(),
                    "); the spill directory may be full"));
    return {};
}

dstd::Result<std::uint64_t> SpillFile::append(const void* data, std::size_t n) {
    const std::uint64_t at = reserve(n);
    auto written = write_at(at, data, n);
    if (!written) return dstd::Unexpected(written.error());
    return at;
}

dstd::Result<void> SpillFile::read(std::uint64_t offset, void* out,
                                   std::size_t n) const {
    auto got = file_.read_exact_at(
        offset, dstd::Span<std::uint8_t>(static_cast<std::uint8_t*>(out), n));
    if (!got)
        return make_error(
            ErrorCode::Io,
            str_cat("spill: read of ", n, " bytes at offset ", offset,
                    " failed (", got.error().format(), ")"));
    return {};
}

}  // namespace dftracer::utils::common
