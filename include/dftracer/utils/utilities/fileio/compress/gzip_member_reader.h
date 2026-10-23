#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_MEMBER_READER_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_MEMBER_READER_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/exception_helpers.h>
#include <dftracer/utils/runtime/coro/async_generator.h>
#include <dftracer/utils/runtime/io/io.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_stream_inflater.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <zlib.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::fileio::compress {

/// Yields each gzip member's decompressed bytes in order, as a string_view into
/// a reused buffer valid until the next step. Boundaries come from libdeflate's
/// consumed-byte count, so no header scanning is needed. A member is decoded
/// whole when it fits `stream_above` bytes (dftracer writes ~16MB members), so
/// peak memory is one decoded member. A member that does not fit is read again
/// from its header with a zlib stream and yielded in steps of whole lines, so
/// memory stays bounded for a member of any size; the libdeflate buffer stops
/// at `stream_above`, so at most that much work is lost per such member. A
/// last member cut short at end of file yields its complete lines when
/// `recover_truncated` is set, and throws like corrupt data when it is not.
inline coro::AsyncGenerator<std::string_view> decode_gzip_members(
    int fd, std::uint64_t file_size,
    std::size_t stream_above = std::size_t{32} << 20,
    bool recover_truncated = true) {
    constexpr std::size_t READ_CHUNK = 1u << 20;
    constexpr std::size_t INIT_OUT = 1u << 20;
    stream_above = std::max(stream_above, INIT_OUT);

    GzipMemberDecompressor dec;
    if (!dec.valid()) {
        throw DFTUtilsException(ErrorCode::Compression,
                                "failed to allocate libdeflate decompressor");
    }

    // Both buffers grow without zero-filling: every byte is written by pread
    // or by the decoder before it is read. A member that does not fit is
    // decoded again from its start, so `out` grows without copying.
    // The last member's ISIZE trailer (its size mod 2^32) sizes `out` up front,
    // as a writer's members are about the same size, so a large member is not
    // decoded once per doubling. Pages of `out` that are never written cost
    // no memory, so a foreign trailer only reserves address space.
    std::size_t out_cap = INIT_OUT;
    if (file_size >= 18) {
        unsigned char trailer[4];
        const auto trailer_read = co_await dftracer::utils::io::pread(
            fd, trailer, sizeof trailer,
            static_cast<off_t>(file_size - sizeof trailer));
        if (trailer_read == static_cast<ssize_t>(sizeof trailer)) {
            const std::size_t isize =
                static_cast<std::size_t>(trailer[0]) |
                static_cast<std::size_t>(trailer[1]) << 8 |
                static_cast<std::size_t>(trailer[2]) << 16 |
                static_cast<std::size_t>(trailer[3]) << 24;
            out_cap =
                std::min(std::max(out_cap, isize + isize / 16), stream_above);
        }
    }
    auto out = std::make_unique_for_overwrite<unsigned char[]>(out_cap);

    // comp holds file bytes [comp_off, next_read) at comp[head, tail).
    std::size_t comp_cap = 0;
    std::size_t head = 0;
    std::size_t tail = 0;
    std::unique_ptr<unsigned char[]> comp;
    std::uint64_t comp_off = 0;
    std::uint64_t next_read = 0;

    while (comp_off < file_size) {
        DecompressResult res{};
        bool decoded = false;
        bool too_big = false;

        while (!decoded && !too_big) {
            if (tail > head) {
                const GzipDecode status = dec.decompress_status(
                    comp.get() + head, tail - head, out.get(), out_cap, res);
                if (status == GzipDecode::Ok) {
                    decoded = true;
                    break;
                }
                if (status == GzipDecode::InsufficientSpace) {
                    if (out_cap >= stream_above) {
                        too_big = true;
                        break;
                    }
                    out_cap = std::min(out_cap * 2, stream_above);
                    out.reset();
                    out = std::make_unique_for_overwrite<unsigned char[]>(
                        out_cap);
                    continue;
                }
                // BadData may just mean the member is not fully buffered yet;
                // pull more input and retry before treating it as corrupt.
            }

            if (next_read >= file_size) {
                // The last member was cut short: yield its complete lines.
                std::vector<std::uint8_t> rest;
                auto keep = recover_truncated
                                ? decode_truncated_member(comp.get() + head,
                                                          tail - head, rest)
                                : std::nullopt;
                if (!keep) {
                    throw DFTUtilsException(ErrorCode::Compression,
                                            "gzip member failed to decompress");
                }
                if (*keep > 0) {
                    co_yield std::string_view(
                        reinterpret_cast<const char*>(rest.data()), *keep);
                }
                co_return;
            }
            // Each retry decodes the member from its start, so at least
            // double the buffered input per retry to stay linear.
            const std::size_t held = tail - head;
            const std::size_t want =
                static_cast<std::size_t>(std::min<std::uint64_t>(
                    std::max(READ_CHUNK, held), file_size - next_read));
            if (tail + want > comp_cap) {
                if (held + want <= comp_cap) {
                    std::memmove(comp.get(), comp.get() + head, held);
                } else {
                    comp_cap = std::max(held + want, comp_cap * 2);
                    auto bigger =
                        std::make_unique_for_overwrite<unsigned char[]>(
                            comp_cap);
                    if (held > 0)
                        std::memcpy(bigger.get(), comp.get() + head, held);
                    comp = std::move(bigger);
                }
                head = 0;
                tail = held;
            }
            const ssize_t n = co_await dftracer::utils::io::pread(
                fd, comp.get() + tail, want, static_cast<off_t>(next_read));
            if (n <= 0) {
                throw DFTUtilsException(
                    ErrorCode::Io, "read error while decoding gzip member");
            }
            tail += static_cast<std::size_t>(n);
            next_read += static_cast<std::uint64_t>(n);
        }

        if (too_big) {
            // Read the member again from its header, a step at a time. A step
            // ends at its last newline; the rest is kept for the next one.
            GzipStreamInflater zs;
            if (!zs.init(31)) {
                throw DFTUtilsException(ErrorCode::Compression,
                                        "failed to start a gzip stream");
            }
            auto in =
                std::make_unique_for_overwrite<unsigned char[]>(READ_CHUNK);
            // The input read so far comes first.
            zs.z().next_in = comp.get() + head;
            zs.z().avail_in = static_cast<uInt>(tail - head);
            std::uint64_t stream_read = next_read;
            std::size_t filled = 0;
            bool ended = false;
            bool cut = false;
            while (!ended) {
                if (zs.z().avail_in == 0 && stream_read < file_size) {
                    const std::size_t want =
                        static_cast<std::size_t>(std::min<std::uint64_t>(
                            READ_CHUNK, file_size - stream_read));
                    const ssize_t n = co_await dftracer::utils::io::pread(
                        fd, in.get(), want, static_cast<off_t>(stream_read));
                    if (n <= 0) {
                        throw DFTUtilsException(
                            ErrorCode::Io,
                            "read error while decoding gzip member");
                    }
                    zs.z().next_in = in.get();
                    zs.z().avail_in = static_cast<uInt>(n);
                    stream_read += static_cast<std::uint64_t>(n);
                }
                zs.z().next_out = out.get() + filled;
                zs.z().avail_out = static_cast<uInt>(out_cap - filled);
                const int rc = zs.inflate(Z_NO_FLUSH);
                filled = out_cap - zs.z().avail_out;
                if (rc == Z_STREAM_END) {
                    ended = true;
                } else if (rc != Z_OK && rc != Z_BUF_ERROR) {
                    throw DFTUtilsException(ErrorCode::Compression,
                                            "gzip member failed to decompress");
                } else if (zs.z().avail_in == 0 && stream_read >= file_size &&
                           zs.z().avail_out != 0) {
                    cut = true;
                    break;
                }
                if (ended || zs.z().avail_out == 0) {
                    std::size_t keep = filled;
                    if (!ended) {
                        while (keep > 0 && out[keep - 1] != '\n') --keep;
                        if (keep == 0) {
                            throw DFTUtilsException(
                                ErrorCode::Compression,
                                "a line is longer than the stream buffer");
                        }
                    }
                    co_yield std::string_view(
                        reinterpret_cast<const char*>(out.get()), keep);
                    std::memmove(out.get(), out.get() + keep, filled - keep);
                    filled -= keep;
                }
            }
            if (cut) {
                if (!recover_truncated) {
                    throw DFTUtilsException(ErrorCode::Compression,
                                            "gzip member failed to decompress");
                }
                std::size_t keep = filled;
                while (keep > 0 && out[keep - 1] != '\n') --keep;
                if (keep > 0) {
                    co_yield std::string_view(
                        reinterpret_cast<const char*>(out.get()), keep);
                }
                co_return;
            }
            comp_off += zs.total_in();
            // The next member is read again from the file.
            head = tail = 0;
            next_read = comp_off;
            continue;
        }

        co_yield std::string_view(reinterpret_cast<const char*>(out.get()),
                                  res.out_bytes);

        comp_off += res.in_bytes;
        head += res.in_bytes;
        if (head == tail) head = tail = 0;
    }
}

}  // namespace dftracer::utils::utilities::fileio::compress

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_MEMBER_READER_H
