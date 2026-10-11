#ifndef DFTRACER_UTILS_INDEX_GZIP_GZIP_MEMBER_SCANNER_H
#define DFTRACER_UTILS_INDEX_GZIP_GZIP_MEMBER_SCANNER_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/io/io.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dftracer::utils::index::gzip {

struct GzipMember {
    std::uint64_t c_offset;  // compressed byte offset of the 1F 8B header
    std::uint64_t c_size;    // compressed size of this member (bytes)
    // ISIZE of the member's trailer: its uncompressed size mod 2^32, or a
    // garbage value when the next candidate is a false positive.
    std::uint32_t isize = 0;
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

inline std::uint32_t load_le32(const unsigned char* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

/// A header candidate and the four bytes before it, which are the ISIZE of
/// the member before when the candidate is real.
struct GzipHeaderHit {
    std::uint64_t offset;
    std::uint32_t before;
};

inline constexpr std::size_t MEMBER_SCAN_WINDOW = std::size_t{4} << 20;
inline constexpr std::uint64_t MEMBER_SCAN_RANGE = std::uint64_t{256} << 20;
inline constexpr std::size_t MEMBER_SCAN_WAYS = 16;

/// Scans the ranges [r * MEMBER_SCAN_RANGE, ...) of `fd` taken from `next`
/// into `hits[r]`; 0 on a read error, else 1. Not bool: when_all writes the
/// results of tasks on different threads into one vector.
inline coro::CoroTask<std::uint8_t> scan_member_ranges(
    int fd, std::uint64_t file_size, std::atomic<std::size_t>* next,
    std::vector<std::vector<GzipHeaderHit>>* hits) {
    // Four bytes behind the window for the ISIZE, nine past it so a header
    // that starts in the window is whole.
    constexpr std::size_t BEHIND = 4;
    constexpr std::size_t AHEAD = 9;
    std::vector<unsigned char> buf(
        BEHIND + std::min<std::uint64_t>(MEMBER_SCAN_WINDOW, file_size) +
        AHEAD);
    for (;;) {
        const std::size_t r = next->fetch_add(1, std::memory_order_relaxed);
        if (r >= hits->size()) co_return 1;
        const std::uint64_t begin = r * MEMBER_SCAN_RANGE;
        const std::uint64_t end =
            std::min<std::uint64_t>(begin + MEMBER_SCAN_RANGE, file_size);
        auto& out = (*hits)[r];
        for (std::uint64_t w = begin; w < end; w += MEMBER_SCAN_WINDOW) {
            const std::uint64_t from = w >= BEHIND ? w - BEHIND : 0;
            const std::uint64_t to = std::min<std::uint64_t>(
                w + MEMBER_SCAN_WINDOW + AHEAD, file_size);
            const std::size_t want = static_cast<std::size_t>(to - from);
            std::size_t got = 0;
            while (got < want) {
                const ssize_t n = co_await dftracer::utils::io::pread(
                    fd, buf.data() + got, want - got,
                    static_cast<off_t>(from + got));
                if (n <= 0) co_return 0;
                got += static_cast<std::size_t>(n);
            }
            const std::uint64_t last = std::min<std::uint64_t>(
                std::min<std::uint64_t>(w + MEMBER_SCAN_WINDOW, end),
                file_size >= 10 ? file_size - 9 : 0);
            const unsigned char* p = buf.data() + (w - from);
            const unsigned char* stop = buf.data() + (last - from);
            while (p < stop) {
                p = static_cast<const unsigned char*>(
                    std::memchr(p, 0x1F, static_cast<std::size_t>(stop - p)));
                if (!p) break;
                if (gzip_header_looks_valid(p)) {
                    const std::uint64_t at =
                        from + static_cast<std::uint64_t>(p - buf.data());
                    out.push_back(
                        {at, at >= BEHIND ? load_le32(p - BEHIND) : 0});
                }
                ++p;
            }
        }
    }
}

/// Scan `fd` for the compressed byte offsets of every candidate gzip header,
/// with up to MEMBER_SCAN_WAYS reads in flight. On return, `out` contains at
/// least one entry (offset 0 if the file starts with a valid gzip header), or
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

    const std::size_t ranges = static_cast<std::size_t>(
        (file_size + MEMBER_SCAN_RANGE - 1) / MEMBER_SCAN_RANGE);
    std::vector<std::vector<GzipHeaderHit>> hits(ranges);
    std::atomic<std::size_t> next{0};
    std::vector<coro::CoroTask<std::uint8_t>> scans;
    const std::size_t ways = std::min(ranges, MEMBER_SCAN_WAYS);
    scans.reserve(ways);
    for (std::size_t i = 0; i < ways; ++i)
        scans.push_back(scan_member_ranges(fd, file_size, &next, &hits));
    const std::vector<std::uint8_t> ok =
        co_await coro::when_all(std::move(scans));
    if (std::find(ok.begin(), ok.end(), 0) != ok.end()) co_return false;

    for (const auto& range : hits)
        for (const auto& h : range) {
            if (!out.empty()) out.back().isize = h.before;
            out.push_back({h.offset, 0});
        }
    if (out.empty()) co_return false;
    unsigned char tail[4];
    // Named, not awaited in the condition (the GCC 12 trap in the coroutine
    // caveats).
    const ssize_t got = co_await dftracer::utils::io::pread(
        fd, tail, sizeof tail, static_cast<off_t>(file_size - 4));
    if (got == 4) out.back().isize = load_le32(tail);

    for (std::size_t i = 0; i + 1 < out.size(); ++i)
        out[i].c_size = out[i + 1].c_offset - out[i].c_offset;
    out.back().c_size = file_size - out.back().c_offset;
    co_return true;
}

}  // namespace dftracer::utils::index::gzip

#endif  // DFTRACER_UTILS_INDEX_GZIP_GZIP_MEMBER_SCANNER_H
