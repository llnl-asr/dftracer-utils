// SPDX-License-Identifier: MIT
#include <dftracer/utils/yaml/escape.h>

#include <array>
#include <cstdint>
#include <cstring>

namespace dftracer::utils::yaml {

namespace {

constexpr std::array<char, 128> make_letters() {
    std::array<char, 128> t{};
    t[static_cast<unsigned char>('"')] = '"';
    t[static_cast<unsigned char>('\\')] = '\\';
    t[static_cast<unsigned char>('\b')] = 'b';
    t[static_cast<unsigned char>('\f')] = 'f';
    t[static_cast<unsigned char>('\n')] = 'n';
    t[static_cast<unsigned char>('\r')] = 'r';
    t[static_cast<unsigned char>('\t')] = 't';
    return t;
}

// The letter after the backslash for an ASCII byte with a short escape.
constexpr std::array<char, 128> LETTER = make_letters();

constexpr bool is_ascii_special(unsigned char u) {
    return LETTER[u] != 0 || u < 0x20 || u == 0x7F;
}

// What a sequence that starts at a byte of 0x80 or more is.
struct Sequence {
    std::size_t length;  // bytes it takes
    bool is_raw;         // written as is
    std::uint32_t code;  // the code point of an escaped one; 0xFFFD if invalid
};

constexpr bool is_continuation(unsigned char b) { return (b & 0xC0) == 0x80; }

Sequence classify(const char* p, const char* end) {
    const unsigned char b0 = static_cast<unsigned char>(*p);
    const std::size_t left = static_cast<std::size_t>(end - p);
    std::size_t length = 0;
    std::uint32_t cp = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        length = 2;
        cp = b0 & 0x1F;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        length = 3;
        cp = b0 & 0x0F;
        if (b0 == 0xE0) low = 0xA0;
        if (b0 == 0xED) high = 0x9F;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        length = 4;
        cp = b0 & 0x07;
        if (b0 == 0xF0) low = 0x90;
        if (b0 == 0xF4) high = 0x8F;
    } else {
        return {1, false, 0xFFFD};
    }
    if (left < length) return {1, false, 0xFFFD};
    for (std::size_t i = 1; i < length; ++i) {
        const unsigned char b = static_cast<unsigned char>(p[i]);
        const bool in_range =
            i == 1 ? (b >= low && b <= high) : is_continuation(b);
        if (!in_range) return {1, false, 0xFFFD};
        cp = (cp << 6) | (b & 0x3F);
    }
    const bool special = (cp >= 0x80 && cp <= 0xA0) || cp == 0x2028 ||
                         cp == 0x2029 || cp == 0xFEFF || cp == 0xFFFE ||
                         cp == 0xFFFF;
    return {length, !special, cp};
}

}  // namespace

std::size_t escaped_yaml_size(dstd::StringView s) {
    std::size_t size = 0;
    const char* p = s.data();
    const char* const end = p + s.size();
    while (p != end) {
        const unsigned char u = static_cast<unsigned char>(*p);
        if (u < 0x80) {
            if (!is_ascii_special(u))
                size += 1;
            else
                size += LETTER[u] != 0 ? 2 : 6;
            ++p;
            continue;
        }
        const Sequence q = classify(p, end);
        size += q.is_raw ? q.length : 6;
        p += q.length;
    }
    return size;
}

char* escape_yaml_to(char* dst, dstd::StringView s) {
    static constexpr char HEX[] = "0123456789abcdef";
    const char* const end = s.data() + s.size();
    const char* run = s.data();
    const char* p = run;
    while (p != end) {
        const unsigned char u = static_cast<unsigned char>(*p);
        std::size_t length = 1;
        std::uint32_t code = u;
        if (u < 0x80) {
            if (!is_ascii_special(u)) {
                ++p;
                continue;
            }
        } else {
            const Sequence q = classify(p, end);
            if (q.is_raw) {
                p += q.length;
                continue;
            }
            length = q.length;
            code = q.code;
        }
        const std::size_t clean = static_cast<std::size_t>(p - run);
        if (clean != 0) {
            std::memcpy(dst, run, clean);
            dst += clean;
        }
        *dst++ = '\\';
        if (u < 0x80 && LETTER[u] != 0) {
            *dst++ = LETTER[u];
        } else {
            *dst++ = 'u';
            *dst++ = HEX[(code >> 12) & 15];
            *dst++ = HEX[(code >> 8) & 15];
            *dst++ = HEX[(code >> 4) & 15];
            *dst++ = HEX[code & 15];
        }
        p += length;
        run = p;
    }
    const std::size_t clean = static_cast<std::size_t>(end - run);
    if (clean != 0) {
        std::memcpy(dst, run, clean);
        dst += clean;
    }
    return dst;
}

}  // namespace dftracer::utils::yaml
