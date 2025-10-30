#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/indexer/internal/checkpoint_size.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

// GZIP header parsing utilities
static bool read_gzip_magic(FILE* f) {
    unsigned char h[3];
    if (std::fseek(f, 0, SEEK_SET) != 0) return false;
    if (std::fread(h, 1, 3, f) != 3) return false;
    return h[0] == 0x1F && h[1] == 0x8B && h[2] == 0x08;  // GZIP magic bytes
}

// Read GZIP ISIZE from last 4 bytes (uncompressed size modulo 2^32)
static bool read_isize_last4(FILE* f, uint32_t& out) {
    if (std::fseek(f, -4, SEEK_END) != 0) return false;
    unsigned char b[4]{};
    if (std::fread(b, 1, 4, f) != 4) return false;
    out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
          ((uint32_t)b[3] << 24);
    return true;
}

// Check if GZIP file contains multiple members (concatenated archives)
static bool likely_multiple_members(FILE* f) {
    if (std::fseek(f, 10, SEEK_SET) != 0) return false;
    constexpr std::size_t BUF = 64 * 1024;
    unsigned char buf[BUF];
    while (true) {
        std::size_t n = std::fread(buf, 1, BUF, f);
        if (!n) break;
        for (std::size_t i = 0; i + 2 < n; ++i)
            if (buf[i] == 0x1F && buf[i + 1] == 0x8B && buf[i + 2] == 0x08)
                return true;
        if (n < BUF) break;
    }
    return false;
}
// Extract filename from GZIP header (if present)
static std::string read_gzip_fname(FILE* f) {
    if (std::fseek(f, 0, SEEK_SET) != 0) return {};
    unsigned char fixed[10];
    if (std::fread(fixed, 1, 10, f) != 10) return {};
    if (!(fixed[0] == 0x1F && fixed[1] == 0x8B && fixed[2] == 0x08)) return {};
    unsigned char flg = fixed[3];
    long pos = 10;
    auto skip = [&](long n) {
        pos += n;
        return std::fseek(f, pos, SEEK_SET) == 0;
    };
    if (flg & 0x04) {
        unsigned char x[2];
        if (std::fread(x, 1, 2, f) != 2) return {};
        uint16_t xlen = (uint16_t)x[0] | ((uint16_t)x[1] << 8);
        pos += 2;
        if (!skip(xlen)) return {};
    }
    std::string name;
    if (flg & 0x08) {
        int c;
        while ((c = std::fgetc(f)) != EOF && c != 0) {
            name.push_back((char)c);
            ++pos;
        }
        ++pos;
    } else {
        if (flg & 0x10) {
            int c;
            while ((c = std::fgetc(f)) != EOF && c != 0) {
                ++pos;
            }
            ++pos;
        }
        if (flg & 0x02) {
            if (!skip(2)) return {};
        }
    }
    return name;
}

// Estimate compression ratio based on file extension
static double guess_text_ratio_from_ext(const std::string& fname) {
    auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    };
    std::string s;
    s.reserve(fname.size());
    for (char c : fname) s.push_back(lower(c));
    auto has = [&](const char* ext) {
        auto p = s.rfind(ext);
        return p != std::string::npos && p + std::strlen(ext) == s.size();
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

static std::size_t align_up(std::size_t x, std::size_t a) {
    return a ? ((x + a - 1) / a) * a : x;
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
        C = std::max(window, std::min(max_chk, align_up(C, window)));
        return C;
    }

    if (S == 0) S = 256u << 10;  // default 256 KiB
    S = std::max(window, std::min(max_chk, align_up(S, window)));

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

namespace dftracer::utils::utilities::indexer::internal {

std::size_t determine_checkpoint_size(std::size_t user_checkpoint_size,
                                      const std::string& path,
                                      std::size_t max_parts,
                                      std::size_t max_chk, std::size_t window) {
    const auto comp_bytes = file_size_bytes(path);

    // Estimate uncompressed size
    std::size_t U = 0;
    if (comp_bytes == 0) {
        // No file info: use user size or default
        std::size_t S =
            user_checkpoint_size ? user_checkpoint_size : (256u << 10);
        S = align_up(std::max(window, S), window);
        return std::min(S, align_down(max_chk, window));
    }

    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        if (read_gzip_magic(f)) {
            uint32_t isize = 0;
            bool has_isize = read_isize_last4(f, isize);
            bool multi = false;
            if (has_isize) {
                std::fseek(f, 0, SEEK_SET);
                multi = likely_multiple_members(f);
            }
            if (has_isize && !multi && isize > 0) {
                U = (std::size_t)isize;
                if (comp_bytes > (4ull << 30) && U < (64ull << 20))
                    U = 0;  // suspect 32-bit wrap
            }
            if (U == 0) {
                std::fseek(f, 0, SEEK_SET);
                std::string fname = read_gzip_fname(f);
                double ratio = guess_text_ratio_from_ext(fname);
                U = std::max<std::size_t>(
                    comp_bytes, (std::size_t)((long double)comp_bytes * ratio));
            }
        } else {
            // uncompressed file
            U = comp_bytes;
        }
        std::fclose(f);
    } else {
        // fallback estimate
        U = std::max<std::size_t>(comp_bytes, comp_bytes * 6);
    }

    // Honor user size if reasonable (<= uncompressed size)
    if (user_checkpoint_size > 0 && U > 0 && user_checkpoint_size <= U) {
        DFTRACER_UTILS_LOG_DEBUG("user_checkpoint_size=%zu\n",
                                 user_checkpoint_size);
        std::size_t S = user_checkpoint_size;
        if (S < window) S = window;
        if (S > max_chk) S = max_chk;
        DFTRACER_UTILS_LOG_DEBUG("final_checkpoint_size=%zu\n", S);
        return S;
    }

    // Calculate optimal checkpoint size
    DFTRACER_UTILS_LOG_DEBUG("comp_bytes=%zu, est_uncomp=%zu\n", comp_bytes, U);
    return choose_divisible_checkpoint(U, user_checkpoint_size, window, max_chk,
                                       max_parts);
}

}  // namespace dftracer::utils::utilities::indexer::internal
