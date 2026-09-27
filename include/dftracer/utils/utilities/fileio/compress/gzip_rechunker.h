#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_RECHUNKER_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_RECHUNKER_H

#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/scoped_fd.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <zlib.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::fileio::compress {

/// The read and index paths decode a whole gzip member at once (libdeflate), so
/// a single foreign member larger than this cannot be handled with bounded
/// memory and must be rechunked first. Matches the reader's default guard.
inline constexpr std::size_t RECHUNK_MEMBER_CAP = std::size_t{1} << 31;

/// True if the file's first gzip member decodes to more than `cap` uncompressed
/// bytes, i.e. it would force an unbounded whole-member decode downstream and
/// must be rechunked first. A bounded first member (whether the file is single-
/// or multi-member) returns false, since the read/index paths handle it fine.
/// Bounded memory: decodes at most one member, discarding output, stopping once
/// output passes `cap`.
inline coro::CoroTask<bool> gzip_needs_rechunk(int fd, std::uint64_t file_size,
                                               std::size_t cap) {
    if (file_size == 0) co_return false;

    z_stream zs{};
    if (inflateInit2(&zs, 15 + 16) != Z_OK) co_return false;

    std::vector<unsigned char> in(1u << 20);
    std::vector<unsigned char> out(1u << 20);
    std::uint64_t off = 0;
    std::uint64_t produced = 0;
    bool result = false;

    while (true) {
        if (zs.avail_in == 0) {
            if (off >= file_size) break;
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(in.size(), file_size - off));
            const ssize_t n = co_await io::pread(fd, in.data(), want,
                                                 static_cast<off_t>(off));
            if (n <= 0) break;
            off += static_cast<std::uint64_t>(n);
            zs.next_in = in.data();
            zs.avail_in = static_cast<uInt>(n);
        }

        zs.next_out = out.data();
        zs.avail_out = static_cast<uInt>(out.size());
        int rc = inflate(&zs, Z_NO_FLUSH);
        produced += out.size() - zs.avail_out;

        if (produced > cap) {  // first member exceeds the cap: rechunk
            result = true;
            break;
        }
        // First member ended within the cap (or corrupt): the fast path is
        // safe. A corrupt/truncated stream is left for the normal path to
        // report, since rechunking would not help.
        if (rc != Z_OK) break;
    }

    inflateEnd(&zs);
    co_return result;
}

/// Decodes `in_path` (single huge member or otherwise; plain text is read as
/// is) with zlib in bounded memory and writes an equivalent multi-member gzip
/// to `out_path`: identical uncompressed bytes, re-framed into libdeflate
/// members of about `member_size` uncompressed bytes cut at newline
/// boundaries.
inline coro::CoroTask<void> gzip_rechunk_to_members(const std::string& in_path,
                                                    const std::string& out_path,
                                                    std::size_t member_size,
                                                    int level) {
    if (member_size == 0)
        member_size = constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    if (level < 0) level = 6;

    ssize_t in_fd_res = co_await io::open(in_path.c_str(), O_RDONLY);
    if (in_fd_res < 0) {
        throw DFTUtilsException(ErrorCode::IO, "Cannot open file: " + in_path);
    }
    ScopedFd in_fd(static_cast<int>(in_fd_res));

    struct stat st;
    if (::fstat(in_fd.get(), &st) != 0) {
        throw DFTUtilsException(ErrorCode::IO, "Cannot stat file: " + in_path);
    }
    const auto file_size = static_cast<std::uint64_t>(st.st_size);
    unsigned char magic[2] = {0, 0};
    const bool plain = co_await io::pread(in_fd.get(), magic, 2, 0) != 2 ||
                       magic[0] != 0x1f || magic[1] != 0x8b;

    ssize_t out_fd_res =
        co_await io::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd_res < 0) {
        throw DFTUtilsException(ErrorCode::IO,
                                "Cannot open output: " + out_path);
    }
    ScopedFd out_fd(static_cast<int>(out_fd_res));

    z_stream zs{};
    if (!plain && inflateInit2(&zs, 15 + 16) != Z_OK) {
        throw DFTUtilsException(ErrorCode::COMPRESSION,
                                "inflateInit2 failed for " + in_path);
    }

    GzipMemberCompressor compressor(level);
    std::vector<unsigned char> in(1u << 20);
    std::vector<unsigned char> out(1u << 20);
    std::vector<char> member;
    std::vector<std::uint8_t> scratch;
    std::uint64_t off = 0;

    auto emit = [&](std::size_t len) -> coro::CoroTask<void> {
        if (!compressor.compress_member_into(scratch, member.data(), len)) {
            throw DFTUtilsException(ErrorCode::COMPRESSION,
                                    "gzip member compression failed");
        }
        std::size_t written = 0;
        while (written < scratch.size()) {
            ssize_t w =
                co_await io::write(out_fd.get(), scratch.data() + written,
                                   scratch.size() - written);
            if (w <= 0) {
                throw DFTUtilsException(ErrorCode::IO,
                                        "write failed: " + out_path);
            }
            written += static_cast<std::size_t>(w);
        }
        member.erase(member.begin(),
                     member.begin() + static_cast<std::ptrdiff_t>(len));
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
            member.insert(member.end(), in.data(), in.data() + n);
        } else {
            if (zs.avail_in == 0) {
                if (off >= file_size) break;
                const std::size_t want = static_cast<std::size_t>(
                    std::min<std::uint64_t>(in.size(), file_size - off));
                const ssize_t n = co_await io::pread(
                    in_fd.get(), in.data(), want, static_cast<off_t>(off));
                if (n <= 0) break;
                off += static_cast<std::uint64_t>(n);
                zs.next_in = in.data();
                zs.avail_in = static_cast<uInt>(n);
            }

            zs.next_out = out.data();
            zs.avail_out = static_cast<uInt>(out.size());
            rc = inflate(&zs, Z_NO_FLUSH);
            const std::size_t got = out.size() - zs.avail_out;
            member.insert(member.end(), out.data(), out.data() + got);
        }

        // Close a member just before a newline once big enough, so the next
        // member begins with that '\n'. The reader's boundary-aware byte range
        // drops bytes up to the first newline of a non-initial member, so a
        // member must lead with the separator (as ChunkWriter emits) or its
        // first event would be dropped on read.
        while (member.size() >= member_size) {
            std::size_t nl = member_size;
            while (nl < member.size() && member[nl] != '\n') ++nl;
            if (nl == member.size()) break;
            co_await emit(nl);
        }

        if (plain) continue;
        if (rc == Z_STREAM_END) {
            if (zs.total_in >= file_size && zs.avail_in == 0) break;
            if (inflateReset2(&zs, 15 + 16) != Z_OK) {
                ok = false;
                break;
            }
        } else if (rc != Z_OK) {
            ok = false;
        }
    }

    if (!plain) inflateEnd(&zs);
    if (!ok) {
        throw DFTUtilsException(ErrorCode::COMPRESSION,
                                "decompression failed for " + in_path);
    }
    if (!member.empty()) co_await emit(member.size());
}

/// If `path`'s first gzip member exceeds `member_size` uncompressed (a single
/// huge member), rechunk it to ~member_size members at `<dir>/<basename>` and
/// return that path; a plain (not gzip) file with content becomes
/// `<dir>/<basename>.gz` the same way. Else return `path` unchanged.
/// Non-destructive; a split copy newer than the source is reused. `did_split`
/// = true when a split copy is returned. `member_size` 0 = default checkpoint
/// size.
inline coro::CoroTask<std::string> rechunk_to_dir_if_needed(
    std::string path, std::string dir, std::size_t member_size,
    bool& did_split) {
    did_split = false;
    ssize_t fd = co_await io::open(path.c_str(), O_RDONLY);
    if (fd < 0) co_return path;
    ScopedFd sfd(static_cast<int>(fd));
    struct stat st;
    if (::fstat(sfd.get(), &st) != 0) co_return path;
    const auto fsize = static_cast<std::uint64_t>(st.st_size);
    if (fsize == 0) co_return path;
    unsigned char magic[2] = {0, 0};
    const bool plain = co_await io::pread(sfd.get(), magic, 2, 0) != 2 ||
                       magic[0] != 0x1f || magic[1] != 0x8b;
    const std::size_t cap = member_size ? member_size : RECHUNK_MEMBER_CAP;
    const bool needs =
        plain || co_await gzip_needs_rechunk(sfd.get(), fsize, cap);
    sfd.reset();
    if (!needs) co_return path;

    std::string split_path = dir + "/" + fs::path(path).filename().string();
    if (plain && !split_path.ends_with(".gz")) split_path += ".gz";
    did_split = true;
    struct stat sst;
    const bool fresh =
        ::stat(split_path.c_str(), &sst) == 0 && sst.st_mtime >= st.st_mtime;
    if (!fresh) {
        fs::create_directories(dir);
        co_await gzip_rechunk_to_members(path, split_path, member_size, 6);
    }
    co_return split_path;
}

}  // namespace dftracer::utils::utilities::fileio::compress

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_COMPRESS_GZIP_RECHUNKER_H
