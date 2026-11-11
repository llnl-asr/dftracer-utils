// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_JSON_JSON_ESCAPE_H
#define DFTRACER_UTILS_JSON_JSON_ESCAPE_H

#include <dftracer/utils/dstd/string.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dftracer::utils::json {

/// Append the JSON-escaped form of `s` (quotes, backslash, the standard
/// control-character shorthands, and `\uXXXX` for every other control
/// character below 0x20, all required by the JSON spec) to `out`. No
/// temporary allocation, so callers building a JSON line in a loop don't
/// churn a string per field.
inline void append_json_escaped(dstd::String& out, dstd::StringView s) {
    static constexpr char HEX[] = "0123456789abcdef";
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default: {
                const unsigned char u = static_cast<unsigned char>(c);
                if (u < 0x20) {
                    const char escaped[6] = {'\\', 'u',         '0',
                                             '0',  HEX[u >> 4], HEX[u & 15]};
                    out.append(escaped, sizeof(escaped));
                } else {
                    out += c;
                }
                break;
            }
        }
    }
}

namespace detail {

/// What a byte becomes in a JSON string: itself, a backslash and one letter,
/// or `\u00XX`.
inline constexpr std::uint8_t ESCAPE_NONE = 0;
inline constexpr std::uint8_t ESCAPE_UNICODE = 1;

constexpr std::array<char, 256> make_escape_letters() {
    std::array<char, 256> t{};
    t[static_cast<unsigned char>('"')] = '"';
    t[static_cast<unsigned char>('\\')] = '\\';
    t[static_cast<unsigned char>('\b')] = 'b';
    t[static_cast<unsigned char>('\f')] = 'f';
    t[static_cast<unsigned char>('\n')] = 'n';
    t[static_cast<unsigned char>('\r')] = 'r';
    t[static_cast<unsigned char>('\t')] = 't';
    return t;
}

/// The letter after the backslash for a byte with a short escape, 0 for any
/// other byte.
inline constexpr std::array<char, 256> ESCAPE_LETTER = make_escape_letters();

/// Bytes an escaped byte adds to its own: 0 for a byte written as is, 1 for a
/// short escape, 5 for `\u00XX`.
constexpr std::array<std::uint8_t, 256> make_escape_extra() {
    std::array<std::uint8_t, 256> t{};
    for (std::size_t i = 0; i < 256; ++i) {
        if (ESCAPE_LETTER[i] != 0)
            t[i] = 1;
        else if (i < 0x20)
            t[i] = 5;
    }
    return t;
}

inline constexpr std::array<std::uint8_t, 256> ESCAPE_EXTRA =
    make_escape_extra();

}  // namespace detail

/// The exact size of the JSON-escaped form of `s`, in one pass over it.
inline std::size_t escaped_json_size(dstd::StringView s) {
    std::size_t extra = 0;
    for (const char c : s)
        extra += detail::ESCAPE_EXTRA[static_cast<unsigned char>(c)];
    return s.size() + extra;
}

/// Like append_json_escaped, into `dst` with room for the escaped size of `s`
/// (at most 6 * s.size() bytes). Copies each run of plain bytes at once.
/// Returns the end of the written bytes.
inline char* escape_json_to(char* dst, dstd::StringView s) {
    static constexpr char HEX[] = "0123456789abcdef";
    const char* const end = s.data() + s.size();
    const char* run = s.data();
    for (const char* p = run; p != end; ++p) {
        const unsigned char u = static_cast<unsigned char>(*p);
        if (detail::ESCAPE_EXTRA[u] == 0) continue;
        const std::size_t clean = static_cast<std::size_t>(p - run);
        if (clean != 0) {
            std::memcpy(dst, run, clean);
            dst += clean;
        }
        run = p + 1;
        *dst++ = '\\';
        if (const char letter = detail::ESCAPE_LETTER[u]; letter != 0) {
            *dst++ = letter;
        } else {
            *dst++ = 'u';
            *dst++ = '0';
            *dst++ = '0';
            *dst++ = HEX[u >> 4];
            *dst++ = HEX[u & 15];
        }
    }
    const std::size_t clean = static_cast<std::size_t>(end - run);
    if (clean != 0) {
        std::memcpy(dst, run, clean);
        dst += clean;
    }
    return dst;
}

/// Escape a string for embedding in a JSON string literal.
inline dstd::String escape_json_string(dstd::StringView s) {
    dstd::String result;
    result.reserve(s.size());
    append_json_escaped(result, s);
    return result;
}

}  // namespace dftracer::utils::json

#endif  // DFTRACER_UTILS_JSON_JSON_ESCAPE_H
