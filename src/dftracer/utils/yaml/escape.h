// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_YAML_ESCAPE_H
#define DFTRACER_UTILS_YAML_ESCAPE_H

#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>

namespace dftracer::utils::yaml {

/// The exact size of the double-quoted YAML form of `s`, without the quotes,
/// in one pass over it.
std::size_t escaped_yaml_size(dstd::StringView s);

/// Writes the double-quoted YAML form of `s`, without the quotes, into `dst`
/// with room for @ref escaped_yaml_size bytes (at most 6 * s.size()). A run of
/// plain bytes is copied at once. Returns the end of the written bytes.
///
/// `"` and `\`, and the C0 controls as the JSON escapes (`\b \f \n \r \t`,
/// `\u00XX` for the rest). Also `\u` for 0x7F, U+0080 to U+00A0, U+2028,
/// U+2029, U+FEFF, U+FFFE and U+FFFF, which a YAML reader takes for a line
/// break or refuses. Every other valid UTF-8 sequence stays raw, the code
/// points above U+FFFF included: YAML `\u` is one code point, so a surrogate
/// pair would read back as two broken characters. A byte that is not valid
/// UTF-8 is written as the escape for U+FFFD, a backslash and `ufffd`.
char* escape_yaml_to(char* dst, dstd::StringView s);

/// Escape policy for dstd::fmt_with: string arguments are escaped for a
/// double-quoted YAML scalar (the quotes are in the format). The bound is the
/// worst case of 6 bytes a byte for a string under LONG_STRING bytes, and the
/// exact escaped size from there on.
struct YamlEscape {
    static constexpr std::size_t LONG_STRING = 64;
    static std::size_t bound(dstd::StringView s) {
        return s.size() < LONG_STRING ? 6 * s.size() : escaped_yaml_size(s);
    }
    static char* write(char* d, dstd::StringView s) {
        return escape_yaml_to(d, s);
    }
};

}  // namespace dftracer::utils::yaml

#endif  // DFTRACER_UTILS_YAML_ESCAPE_H
