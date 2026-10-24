#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_RECHUNKER_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_RECHUNKER_H

#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/common/scoped_fd.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/runtime/io/io.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_stream_inflater.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <zlib.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::fileio::compress {

/// A member larger than this is worth rewriting as smaller members: the
/// readers stream it, but a multi-member file is read faster and in parallel.
inline constexpr std::size_t RECHUNK_MEMBER_CAP = std::size_t{1} << 31;

/// What decoding the start of a file shows. `compressed` and `uncompressed`
/// count the bytes read and produced up to the point where the decode stopped.
struct FirstMember {
    bool exceeds_cap = false;
    std::uint64_t compressed = 0;
    std::uint64_t uncompressed = 0;
};

/// Decodes the first gzip member of the file, discarding the output, and stops
/// once the output passes `cap`. Bounded memory: one member at most, never
/// held. With `sample_bytes` the decode goes on through the next members until
/// it has read that many compressed bytes, so a tiny first member (a "[" line
/// of its own) does not decide the ratio. `exceeds_cap` is about the first
/// member only.
inline coro::CoroTask<FirstMember> gzip_first_member(
    int fd, std::uint64_t file_size, std::size_t cap,
    std::uint64_t sample_bytes = 0) {
    FirstMember member;
    if (file_size == 0) co_return member;

    GzipStreamInflater gz;
    if (!gz.init(15 + 16)) co_return member;
    z_stream& zs = gz.z();

    constexpr std::size_t BUFFER_BYTES = std::size_t{1} << 20;
    const std::size_t in_bytes = static_cast<std::size_t>(
        std::min<std::uint64_t>(BUFFER_BYTES, file_size));
    auto in = std::make_unique_for_overwrite<unsigned char[]>(in_bytes);
    auto out = std::make_unique_for_overwrite<unsigned char[]>(BUFFER_BYTES);
    std::uint64_t off = 0;
    std::uint64_t produced = 0;
    std::uint64_t member_produced = 0;
    std::uint64_t finished = 0;  // compressed bytes of the members that ended
    bool in_first = true;
    bool counted = false;

    while (true) {
        if (zs.avail_in == 0) {
            if (off >= file_size) break;
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(in_bytes, file_size - off));
            const ssize_t n =
                co_await io::pread(fd, in.get(), want, static_cast<off_t>(off));
            if (n <= 0) break;
            off += static_cast<std::uint64_t>(n);
            zs.next_in = in.get();
            zs.avail_in = static_cast<uInt>(n);
        }

        zs.next_out = out.get();
        zs.avail_out = static_cast<uInt>(BUFFER_BYTES);
        int rc = gz.inflate(Z_NO_FLUSH);
        const std::uint64_t got = BUFFER_BYTES - zs.avail_out;
        produced += got;
        member_produced += got;

        if (in_first && member_produced > cap) {  // rechunk
            member.exceeds_cap = true;
            break;
        }
        if (rc == Z_STREAM_END) {
            finished += gz.total_in();
            in_first = false;
            const bool more = zs.avail_in > 0 || off < file_size;
            if (finished >= sample_bytes || !more || !gz.reset()) {
                counted = true;
                break;
            }
            member_produced = 0;
            continue;
        }
        // The first member ended within the cap (or is corrupt): the fast path
        // is safe. A corrupt or truncated stream is left for the normal path
        // to report, since rechunking would not help.
        if (rc != Z_OK) break;
    }

    member.compressed = counted ? finished : finished + gz.total_in();
    member.uncompressed = produced;
    co_return member;
}

/// Decodes `in_path` (single huge member or otherwise; plain text is read as
/// is) with zlib in bounded memory and writes an equivalent multi-member gzip
/// to `out_path`: identical uncompressed bytes, re-framed by GzipLineWriter
/// into members of about `member_size` uncompressed bytes that end at newlines.
inline coro::CoroTask<void> gzip_rechunk_to_members(const std::string& in_path,
                                                    const std::string& out_path,
                                                    std::size_t member_size,
                                                    int level) {
    if (member_size == 0)
        member_size = constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    if (level < 0) level = 6;

    ssize_t in_fd_res = co_await io::open(in_path.c_str(), O_RDONLY);
    if (in_fd_res < 0) {
        throw DFTUtilsException(ErrorCode::Io, "Cannot open file: " + in_path);
    }
    ScopedFd in_fd(static_cast<int>(in_fd_res));

    struct stat st;
    if (::fstat(in_fd.get(), &st) != 0) {
        throw DFTUtilsException(ErrorCode::Io, "Cannot stat file: " + in_path);
    }
    const auto file_size = static_cast<std::uint64_t>(st.st_size);
    unsigned char magic[2] = {0, 0};
    const bool plain = co_await io::pread(in_fd.get(), magic, 2, 0) != 2 ||
                       magic[0] != 0x1f || magic[1] != 0x8b;

    GzipStreamInflater gz;
    z_stream& zs = gz.z();
    if (!plain && !gz.init(15 + 16)) {
        throw DFTUtilsException(ErrorCode::Compression,
                                "inflateInit2 failed for " + in_path);
    }

    GzipWriterOptions opts;
    opts.member_size = member_size;
    opts.level = level;
    auto writer = unwrap(co_await GzipLineWriter::open(out_path, opts));

    std::vector<unsigned char> in(1u << 20);
    std::vector<unsigned char> out(1u << 20);
    std::string carry;
    std::uint64_t off = 0;

    // Appends the whole lines of `text` and keeps the rest in `carry`.
    auto feed = [&](std::string_view text) -> coro::CoroTask<void> {
        const auto nl = text.rfind('\n');
        if (nl == std::string_view::npos) {
            carry.append(text);
            co_return;
        }
        carry.append(text.substr(0, nl + 1));
        unwrap(co_await writer.append(carry));
        carry.assign(text.substr(nl + 1));
    };

    bool ok = true;
    while (ok) {
        int rc = Z_OK;
        if (plain) {
            if (off >= file_size) break;
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(in.size(), file_size - off));
            const ssize_t n = co_await io::pread(in_fd.get(), in.data(), want,
                                                 static_cast<off_t>(off));
            if (n <= 0) break;
            off += static_cast<std::uint64_t>(n);
            co_await feed(
                std::string_view(reinterpret_cast<const char*>(in.data()),
                                 static_cast<std::size_t>(n)));
            continue;
        }
        if (zs.avail_in == 0) {
            if (off >= file_size) break;
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(in.size(), file_size - off));
            const ssize_t n = co_await io::pread(in_fd.get(), in.data(), want,
                                                 static_cast<off_t>(off));
            if (n <= 0) break;
            off += static_cast<std::uint64_t>(n);
            zs.next_in = in.data();
            zs.avail_in = static_cast<uInt>(n);
        }

        zs.next_out = out.data();
        zs.avail_out = static_cast<uInt>(out.size());
        rc = gz.inflate(Z_NO_FLUSH);
        co_await feed(
            std::string_view(reinterpret_cast<const char*>(out.data()),
                             out.size() - zs.avail_out));

        if (rc == Z_STREAM_END) {
            if (gz.total_in() >= file_size && zs.avail_in == 0) break;
            if (!gz.reset()) {
                ok = false;
                break;
            }
        } else if (rc != Z_OK) {
            ok = false;
        }
    }

    if (!ok) {
        throw DFTUtilsException(ErrorCode::Compression,
                                "decompression failed for " + in_path);
    }
    unwrap(co_await writer.close(carry));
}

}  // namespace dftracer::utils::utilities::fileio::compress

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_RECHUNKER_H
