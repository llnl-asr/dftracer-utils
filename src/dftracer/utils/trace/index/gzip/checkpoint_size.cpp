// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/bits.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/trace/index/gzip/checkpoint_size.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>

using namespace dftracer::utils;

// GZIP header parsing utilities
static bool read_gzip_magic(const dstd::File& file) {
    unsigned char h[3];
    const auto n = file.read_at(0, dstd::Span<std::uint8_t>(h, 3));
    if (!n || *n != 3) return false;
    return h[0] == 0x1F && h[1] == 0x8B && h[2] == 0x08;
}

// Read GZIP ISIZE from last 4 bytes (uncompressed size modulo 2^32)
static bool read_isize_last4(const dstd::File& file, uint32_t& out) {
    const auto size = file.size();
    if (!size || *size < 4) return false;
    unsigned char b[4]{};
    const auto n = file.read_at(*size - 4, dstd::Span<std::uint8_t>(b, 4));
    if (!n || *n != 4) return false;
    out = dftracer::utils::dstd::read_u32_le(b);
    return true;
}

// Check if GZIP file contains multiple members (concatenated archives)
static bool likely_multiple_members(const dstd::File& file) {
    constexpr std::size_t BUF = 64 * 1024;
    unsigned char buf[BUF];
    std::uint64_t offset = 10;
    while (true) {
        const auto got =
            file.read_at(offset, dstd::Span<std::uint8_t>(buf, BUF));
        if (!got || *got == 0) break;
        const std::size_t n = *got;
        for (std::size_t i = 0; i + 2 < n; ++i)
            if (buf[i] == 0x1F && buf[i + 1] == 0x8B && buf[i + 2] == 0x08)
                return true;
        offset += n;
        if (n < BUF) break;
    }
    return false;
}
// Reads one byte at `pos`; false at the end of the file or on error.
static bool read_byte_at(const dstd::File& file, std::uint64_t pos,
                         unsigned char& out) {
    const auto n = file.read_at(pos, dstd::Span<std::uint8_t>(&out, 1));
    return n && *n == 1;
}

// Extract filename from GZIP header (if present)
static dstd::String read_gzip_fname(const dstd::File& file) {
    unsigned char fixed[10];
    const auto head = file.read_at(0, dstd::Span<std::uint8_t>(fixed, 10));
    if (!head || *head != 10) return {};
    if (!(fixed[0] == 0x1F && fixed[1] == 0x8B && fixed[2] == 0x08)) return {};
    unsigned char flg = fixed[3];
    std::uint64_t pos = 10;
    if (flg & 0x04) {
        unsigned char x[2];
        const auto n = file.read_at(pos, dstd::Span<std::uint8_t>(x, 2));
        if (!n || *n != 2) return {};
        auto xlen = dftracer::utils::dstd::read_u16_le(x);
        pos += 2 + xlen;
    }
    dstd::String name;
    if (flg & 0x08) {
        unsigned char c;
        while (read_byte_at(file, pos, c) && c != 0) {
            name.push_back(static_cast<char>(c));
            ++pos;
        }
        ++pos;
    } else {
        if (flg & 0x10) {
            unsigned char c;
            while (read_byte_at(file, pos, c) && c != 0) {
                ++pos;
            }
            ++pos;
        }
        if (flg & 0x02) {
            pos += 2;
        }
    }
    return name;
}

// Estimate compression ratio based on file extension
static double guess_text_ratio_from_ext(const dstd::String& fname) {
    dstd::String s;
    s.reserve(fname.size());
    for (char c : fname) s.push_back(dstd::to_ascii_lower(c));
    auto has = [&](const char* ext) {
        auto p = s.rfind(ext);
        return p != dstd::String::npos && p + std::strlen(ext) == s.size();
    };
    if (has(".pfw") || has(".json") || has(".jsonl") || has(".ndjson") ||
        has(".txt") || has(".csv"))
        return 8.0;
    return 6.0;
}

// Size alignment utilities
static std::size_t align_down(std::size_t x, std::size_t a) {
    return a ? (x / a) * a : x;
}

// Calculate optimal checkpoint size that divides uncompressed size evenly
static std::size_t choose_divisible_checkpoint(
    std::size_t U, std::size_t S,
    std::size_t window,                   // lower bound + multiple
    std::size_t max_chk, std::size_t max_parts) {
    if (window == 0) window = 32u << 10;  // safety
    if (U == 0) {
        // No size info: use user size or default
        std::size_t C = (S ? S : (16u << 10));
        C = std::max(
            window, std::min(max_chk,
                             dftracer::utils::dstd::bits::align_up(C, window)));
        return C;
    }

    if (S == 0) S = 256u << 10;  // default 256 KiB
    S = std::max(
        window,
        std::min(max_chk, dftracer::utils::dstd::bits::align_up(S, window)));

    long double ratio = (long double)U / (long double)S;
    std::size_t k = (std::size_t)(ratio + 0.5L);
    if (k == 0) k = 1;
    if (k > max_parts) k = max_parts;

    // Calculate checkpoint size as U/k, aligned to window boundary
    std::size_t C = U / k;
    if (C == 0) {
        C = window;
        k = (U >= window) ? (U / window) : 1;
    }
    C = align_down(C, window);
    if (C < window) C = window;

    // Apply maximum size limit
    if (C > max_chk) C = align_down(max_chk, window);

    // Ensure total doesn't exceed uncompressed size
    if ((unsigned long long)k * (unsigned long long)C > (unsigned long long)U) {
        k = std::max<std::size_t>(1, U / C);
    }

    // Handle tiny files
    if (U < window) C = window;

    return C;
}

namespace dftracer::utils::trace::index::gzip {

std::size_t determine_checkpoint_size(std::size_t user_checkpoint_size,
                                      dstd::CStringView path,
                                      std::size_t max_parts,
                                      std::size_t max_chk, std::size_t window) {
    const auto comp_bytes = fileio::file_size_bytes(path);

    // Estimate uncompressed size
    std::size_t U = 0;
    if (comp_bytes == 0) {
        // No file info: use user size or default
        std::size_t S =
            user_checkpoint_size ? user_checkpoint_size : (256u << 10);
        S = dftracer::utils::dstd::bits::align_up(std::max(window, S), window);
        return std::min(S, align_down(max_chk, window));
    }

    auto file = dstd::File::open(dstd::PathView(path), {});
    if (file) {
        if (read_gzip_magic(*file)) {
            uint32_t isize = 0;
            bool has_isize = read_isize_last4(*file, isize);
            bool multi = false;
            if (has_isize) {
                multi = likely_multiple_members(*file);
            }
            if (has_isize && !multi && isize > 0) {
                U = (std::size_t)isize;
                if (comp_bytes > (4ull << 30) && U < (64ull << 20))
                    U = 0;  // suspect 32-bit wrap
            }
            if (U == 0) {
                // The size only picks a checkpoint size, never the results.
                dstd::String fname = read_gzip_fname(*file);
                double ratio = guess_text_ratio_from_ext(fname);
                DFTRACER_UTILS_LOG_DEBUG(
                    "checkpoint size: {} has no usable uncompressed size, "
                    "assume ratio {:.1f}",
                    path, ratio);
                U = std::max<std::size_t>(
                    comp_bytes, (std::size_t)((long double)comp_bytes * ratio));
            }
        } else {
            // uncompressed file
            U = comp_bytes;
        }
    } else {
        // The size only picks a checkpoint size, never the results.
        DFTRACER_UTILS_LOG_DEBUG(
            "checkpoint size: cannot open {}, assume a ratio of 6", path);
        U = std::max<std::size_t>(comp_bytes, comp_bytes * 6);
    }

    // Honor user size if reasonable (<= uncompressed size)
    if (user_checkpoint_size > 0 && U > 0 && user_checkpoint_size <= U) {
        DFTRACER_UTILS_LOG_DEBUG("user_checkpoint_size={}\n",
                                 user_checkpoint_size);
        std::size_t S = user_checkpoint_size;
        if (S < window) S = window;
        if (S > max_chk) S = max_chk;
        DFTRACER_UTILS_LOG_DEBUG("final_checkpoint_size={}\n", S);
        return S;
    }

    // Calculate optimal checkpoint size
    DFTRACER_UTILS_LOG_DEBUG("comp_bytes={}, est_uncomp={}\n", comp_bytes, U);
    return choose_divisible_checkpoint(U, user_checkpoint_size, window, max_chk,
                                       max_parts);
}

}  // namespace dftracer::utils::trace::index::gzip
