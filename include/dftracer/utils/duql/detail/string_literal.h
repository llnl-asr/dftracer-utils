// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_DETAIL_STRING_LITERAL_H
#define DFTRACER_UTILS_DUQL_DETAIL_STRING_LITERAL_H

#include <dftracer/utils/dstd/hex.h>
#include <dftracer/utils/dstd/string.h>

namespace dftracer::utils::duql::detail {

/// `v` as a duql string literal that reads back as exactly `v`: in double
/// quotes, with `\\`, `\"`, `\n`, `\t`, `\r` and `\u00XX` for the other
/// control bytes. Other bytes, UTF-8 included, pass through.
inline dstd::String quote_string(dstd::StringView v) {
    dstd::String out;
    out.reserve(v.size() + 2);
    out += '"';
    for (const char ch : v) {
        const auto b = static_cast<unsigned char>(ch);
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\r':
                out += "\\r";
                break;
            default:
                if (b < 0x20 || b == 0x7f) {
                    out += "\\u00";
                    out += dstd::HEX_LOWER[b >> 4];
                    out += dstd::HEX_LOWER[b & 0xf];
                } else {
                    out += ch;
                }
        }
    }
    out += '"';
    return out;
}

}  // namespace dftracer::utils::duql::detail

#endif  // DFTRACER_UTILS_DUQL_DETAIL_STRING_LITERAL_H
