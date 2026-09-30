#ifndef DFTRACER_UTILS_DUQL_STRING_LITERAL_H
#define DFTRACER_UTILS_DUQL_STRING_LITERAL_H

#include <string>
#include <string_view>

namespace dftracer::utils::duql {

/// `v` as a duql string literal that reads back as exactly `v`: in double
/// quotes, with `\\`, `\"`, `\n`, `\t`, `\r` and `\u00XX` for the other
/// control bytes. Other bytes, UTF-8 included, pass through.
inline std::string quote_string(std::string_view v) {
    static constexpr char HEX[] = "0123456789abcdef";
    std::string out;
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
                    out += HEX[b >> 4];
                    out += HEX[b & 0xf];
                } else {
                    out += ch;
                }
        }
    }
    out += '"';
    return out;
}

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_STRING_LITERAL_H
