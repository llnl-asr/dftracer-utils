// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_MEMBER_READER_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_MEMBER_READER_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/exception_helpers.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/io/io.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_stream_inflater.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dftracer::utils::utilities::fileio::compress {

/// Yields each gzip member's decompressed bytes in order, as a string_view into
/// a reused buffer valid until the next step. A member over `stream_above`
/// bytes is streamed in steps of whole lines, losing at most that much work. A
/// last member cut short yields its complete lines when `recover_truncated` is
/// set, else it throws.
inline coro::AsyncGenerator<dstd::StringView> decode_gzip_members(
    int fd, std::uint64_t file_size,
    std::size_t stream_above = std::size_t{32} << 20,
    bool recover_truncated = true) {
    constexpr std::size_t READ_CHUNK = 1u << 20;
    constexpr std::size_t INIT_OUT = 1u << 20;
    stream_above = std::max(stream_above, INIT_OUT);

    GzipMemberDecompressor dec;
    if (!dec.valid()) {
        throw common::DFTUtilsException(
            common::ErrorCode::Compression,
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
        const auto trailer_read = co_await dftracer::utils::runtime::io::pread(
            fd, trailer, sizeof trailer, file_size - sizeof trailer);
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
    auto out = dstd::make_unique_for_overwrite<unsigned char[]>(out_cap);

    // comp holds file bytes [comp_off, next_read) at comp[head, tail).
    std::size_t comp_cap = 0;
    std::size_t head = 0;
    std::size_t tail = 0;
    dstd::UniquePtr<unsigned char[]> comp;
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
                    out = dstd::make_unique_for_overwrite<unsigned char[]>(
                        out_cap);
                    continue;
                }
                // BadData may just mean the member is not fully buffered yet;
                // pull more input and retry before treating it as corrupt.
            }

            if (next_read >= file_size) {
                // The last member was cut short: yield its complete lines.
                dstd::Vector<std::uint8_t> rest;
                auto keep = recover_truncated
                                ? decode_truncated_member(comp.get() + head,
                                                          tail - head, rest)
                                : dstd::nullopt;
                if (!keep) {
                    throw common::DFTUtilsException(
                        common::ErrorCode::Compression,
                        "gzip member failed to decompress");
                }
                if (*keep > 0) {
                    co_yield dstd::StringView(
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
                        dstd::make_unique_for_overwrite<unsigned char[]>(
                            comp_cap);
                    if (held > 0)
                        std::memcpy(bigger.get(), comp.get() + head, held);
                    comp = std::move(bigger);
                }
                head = 0;
                tail = held;
            }
            const ssize_t n = co_await dftracer::utils::runtime::io::pread(
                fd, comp.get() + tail, want, next_read);
            if (n <= 0) {
                throw common::DFTUtilsException(
                    common::ErrorCode::Io,
                    "read error while decoding gzip member");
            }
            tail += static_cast<std::size_t>(n);
            next_read += static_cast<std::uint64_t>(n);
        }

        if (too_big) {
            // A step ends at its last newline. The rest is kept for the next
            // one.
            GzipStreamInflater zs;
            if (!zs.init(31)) {
                throw common::DFTUtilsException(
                    common::ErrorCode::Compression,
                    "failed to start a gzip stream");
            }
            auto in =
                dstd::make_unique_for_overwrite<unsigned char[]>(READ_CHUNK);
            zs.set_input(comp.get() + head, tail - head);
            std::uint64_t stream_read = next_read;
            std::size_t filled = 0;
            bool ended = false;
            bool cut = false;
            while (!ended) {
                if (zs.avail_in() == 0 && stream_read < file_size) {
                    const std::size_t want =
                        static_cast<std::size_t>(std::min<std::uint64_t>(
                            READ_CHUNK, file_size - stream_read));
                    const ssize_t n =
                        co_await dftracer::utils::runtime::io::pread(
                            fd, in.get(), want, stream_read);
                    if (n <= 0) {
                        throw common::DFTUtilsException(
                            common::ErrorCode::Io,
                            "read error while decoding gzip member");
                    }
                    zs.set_input(in.get(), static_cast<std::size_t>(n));
                    stream_read += static_cast<std::uint64_t>(n);
                }
                zs.set_output(out.get() + filled, out_cap - filled);
                const InflateStatus rc = zs.inflate();
                filled = out_cap - zs.avail_out();
                if (rc == InflateStatus::StreamEnd) {
                    ended = true;
                } else if (rc != InflateStatus::Ok &&
                           rc != InflateStatus::BufError) {
                    throw common::DFTUtilsException(
                        common::ErrorCode::Compression,
                        "gzip member failed to decompress");
                } else if (zs.avail_in() == 0 && stream_read >= file_size &&
                           zs.avail_out() != 0) {
                    cut = true;
                    break;
                }
                if (ended || zs.avail_out() == 0) {
                    std::size_t keep = filled;
                    if (!ended) {
                        while (keep > 0 && out[keep - 1] != '\n') --keep;
                        if (keep == 0) {
                            throw common::DFTUtilsException(
                                common::ErrorCode::Compression,
                                "a line is longer than the stream buffer");
                        }
                    }
                    co_yield dstd::StringView(
                        reinterpret_cast<const char*>(out.get()), keep);
                    std::memmove(out.get(), out.get() + keep, filled - keep);
                    filled -= keep;
                }
            }
            if (cut) {
                if (!recover_truncated) {
                    throw common::DFTUtilsException(
                        common::ErrorCode::Compression,
                        "gzip member failed to decompress");
                }
                std::size_t keep = filled;
                while (keep > 0 && out[keep - 1] != '\n') --keep;
                if (keep > 0) {
                    co_yield dstd::StringView(
                        reinterpret_cast<const char*>(out.get()), keep);
                }
                co_return;
            }
            comp_off += zs.total_in();
            head = tail = 0;
            next_read = comp_off;
            continue;
        }

        co_yield dstd::StringView(reinterpret_cast<const char*>(out.get()),
                                  res.out_bytes);

        comp_off += res.in_bytes;
        head += res.in_bytes;
        if (head == tail) head = tail = 0;
    }
}

}  // namespace dftracer::utils::utilities::fileio::compress

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_MEMBER_READER_H
