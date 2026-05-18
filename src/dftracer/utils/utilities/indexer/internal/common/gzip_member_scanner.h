#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_COMMON_GZIP_MEMBER_SCANNER_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_COMMON_GZIP_MEMBER_SCANNER_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/io.h>
#include <sys/stat.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dftracer::utils::utilities::indexer::internal {

struct GzipMember {
    std::uint64_t c_offset;  // compressed byte offset of the 1F 8B header
    std::uint64_t c_size;    // compressed size of this member (bytes)
};

/// Cheap validation of a 10-byte gzip header candidate starting at buf[i].
/// Rejects patterns that look like 1F 8B 08 but don't parse as a real
/// gzip header (FLG reserved bits, XFL, OS sanity). Callers must have
/// `end - buf[i] >= 10`.
inline bool gzip_header_looks_valid(const unsigned char* buf) noexcept {
    if (buf[0] != 0x1F || buf[1] != 0x8B || buf[2] != 0x08) return false;
    const unsigned char flg = buf[3];
    if (flg & 0xE0) return false;  // reserved bits must be zero
    const unsigned char xfl = buf[8];
    if (xfl != 0 && xfl != 2 && xfl != 4) return false;
    const unsigned char os = buf[9];
    if (os > 13 && os != 255) return false;
    return true;
}

/// Scan `fd` in buffered pread windows, collecting compressed byte offsets
/// of every candidate gzip header. On return, `out` contains at least
/// one entry (offset 0 if the file starts with a valid gzip header), or
/// is empty if the file is not a gzip stream. `c_size` is populated as
/// the gap to the next member's offset; the last member's `c_size`
/// extends to file end.
///
/// False positives (the byte pattern appearing inside compressed data
/// with a plausible header) are possible; callers must treat a returned
/// list as "candidate" and validate at inflate time.
inline coro::CoroTask<bool> enumerate_gzip_member_candidates(
    int fd, std::uint64_t file_size, std::vector<GzipMember>& out) {
    out.clear();
    if (file_size < 18)
        co_return false;  // min gzip: 10 header + 2 deflate + 8 trailer

    // Window size tuned for Lustre sequential reads; keep a small overlap
    // so a header straddling a window boundary is still seen by the scan.
    constexpr std::size_t WIN = 1 << 20;  // 1 MiB
    constexpr std::size_t OVERLAP = 16;   // >= gzip fixed header size

    std::vector<unsigned char> buf(WIN);
    std::uint64_t pos = 0;
    std::uint64_t carry = 0;  // how many bytes at buf[0..carry) are stale

    while (pos < file_size) {
        const std::size_t want =
            std::min<std::uint64_t>(WIN - carry, file_size - pos);
        ssize_t n = co_await dftracer::utils::io::pread(
            fd, buf.data() + carry, want, static_cast<off_t>(pos));
        if (n <= 0) {
            if (out.empty()) co_return false;
            break;
        }
        const std::size_t avail = carry + static_cast<std::size_t>(n);
        const std::uint64_t base = pos - carry;

        const std::size_t scan_end = (avail >= 10) ? (avail - 9) : 0;
        for (std::size_t i = 0; i < scan_end; ++i) {
            if (buf[i] != 0x1F) continue;
            if (gzip_header_looks_valid(buf.data() + i)) {
                out.push_back({base + i, 0});
            }
        }

        pos += static_cast<std::uint64_t>(n);

        // Copy the trailing OVERLAP bytes to the front so a header
        // straddling the next read is still caught.
        if (avail >= OVERLAP) {
            std::memmove(buf.data(), buf.data() + avail - OVERLAP, OVERLAP);
            carry = OVERLAP;
        } else {
            carry = avail;
            std::memmove(buf.data(), buf.data() + (avail - carry), carry);
        }
    }

    if (out.empty()) co_return false;

    // Fill c_size: gap between consecutive candidates; last one extends to EOF.
    for (std::size_t i = 0; i + 1 < out.size(); ++i) {
        out[i].c_size = out[i + 1].c_offset - out[i].c_offset;
    }
    out.back().c_size = file_size - out.back().c_offset;

    co_return true;
}

}  // namespace dftracer::utils::utilities::indexer::internal

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_COMMON_GZIP_MEMBER_SCANNER_H
