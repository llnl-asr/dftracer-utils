#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_MEMBER_READER_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_MEMBER_READER_H

#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/exception_helpers.h>
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::fileio::compress {

/// Yields each gzip member's decompressed bytes in order, as a string_view into
/// a reused buffer valid until the next step. Boundaries come from libdeflate's
/// consumed-byte count, so no header scanning is needed. Peak memory is one
/// decoded member: dftracer emits ~16MB members, so this is bounded in
/// practice. A single foreign member whose decoded size exceeds
/// `max_member_bytes` throws instead of decoding the whole file into memory;
/// the caller should re-chunk it with dftracer_split. A last member cut short
/// at end of file yields its complete lines when `recover_truncated` is set,
/// and throws like corrupt data when it is not.
inline coro::AsyncGenerator<std::string_view> decode_gzip_members(
    int fd, std::uint64_t file_size,
    std::size_t max_member_bytes = std::size_t{1} << 31,
    bool recover_truncated = true) {
    constexpr std::size_t READ_CHUNK = 1u << 20;
    constexpr std::size_t INIT_OUT = 1u << 20;

    GzipMemberDecompressor dec;
    if (!dec.valid()) {
        throw DFTUtilsException(ErrorCode::COMPRESSION,
                                "failed to allocate libdeflate decompressor");
    }

    // comp holds file bytes [comp_off, next_read).
    std::vector<unsigned char> comp;
    std::vector<unsigned char> out(INIT_OUT);
    std::uint64_t comp_off = 0;
    std::uint64_t next_read = 0;

    while (comp_off < file_size) {
        DecompressResult res{};
        bool decoded = false;

        while (!decoded) {
            if (!comp.empty()) {
                const GzipDecode status = dec.decompress_status(
                    comp.data(), comp.size(), out.data(), out.size(), res);
                if (status == GzipDecode::Ok) {
                    decoded = true;
                    break;
                }
                if (status == GzipDecode::InsufficientSpace) {
                    if (out.size() >= max_member_bytes) {
                        throw DFTUtilsException(
                            ErrorCode::COMPRESSION,
                            "gzip member is too large to decode in memory; "
                            "re-chunk the file with dftracer_split");
                    }
                    out.resize(std::min(out.size() * 2, max_member_bytes));
                    continue;
                }
                // BadData may just mean the member is not fully buffered yet;
                // pull more input and retry before treating it as corrupt.
            }

            if (next_read >= file_size) {
                // The last member was cut short: yield its complete lines.
                auto keep =
                    recover_truncated
                        ? decode_truncated_member(comp.data(), comp.size(), out)
                        : std::nullopt;
                if (!keep) {
                    throw DFTUtilsException(ErrorCode::COMPRESSION,
                                            "gzip member failed to decompress");
                }
                if (*keep > 0) {
                    co_yield std::string_view(
                        reinterpret_cast<const char*>(out.data()), *keep);
                }
                co_return;
            }
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(READ_CHUNK, file_size - next_read));
            const std::size_t old = comp.size();
            comp.resize(old + want);
            const ssize_t n = co_await dftracer::utils::io::pread(
                fd, comp.data() + old, want, static_cast<off_t>(next_read));
            if (n <= 0) {
                throw DFTUtilsException(
                    ErrorCode::IO, "read error while decoding gzip member");
            }
            comp.resize(old + static_cast<std::size_t>(n));
            next_read += static_cast<std::uint64_t>(n);
        }

        co_yield std::string_view(reinterpret_cast<const char*>(out.data()),
                                  res.out_bytes);

        comp_off += res.in_bytes;
        comp.erase(comp.begin(),
                   comp.begin() + static_cast<std::ptrdiff_t>(res.in_bytes));
    }
}

}  // namespace dftracer::utils::utilities::fileio::compress

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_MEMBER_READER_H
