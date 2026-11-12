// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_REFUSED_LINES_H
#define DFTRACER_UTILS_TESTS_REFUSED_LINES_H

#include <dftracer/utils/dstd/string.h>

namespace dftu_utils_test {

/// The kinds of line refused_line makes.
inline constexpr int REFUSED_KINDS = 12;

/// One JSON member that the DOM parser refuses, one per kind: a number too
/// large for a double, an invalid escape, a 24-digit integer, a bad UTF-8 byte,
/// a raw tab in a string, a nesting of 1100 levels, then integers of 20, 21
/// and 22 digits and the same three negative.
inline dftracer::utils::dstd::String refused_member(int kind) {
    using dftracer::utils::dstd::String;
    String odd;
    switch (kind) {
        case 0:
            odd = R"("n":1e400)";
            break;
        case 1:
            odd = R"("s":"a\qb")";
            break;
        case 2:
            odd = R"("n":123456789012345678901234)";
            break;
        case 3:
            odd =
                "\"s\":\"x\xff"
                "y\"";
            break;
        case 4:
            odd = "\"s\":\"a\tb\"";
            break;
        case 5:
            odd = R"("deep":)";
            odd.append(1100, '[');
            odd.append(1100, ']');
            break;
        default: {
            const int width = 20 + (kind - 6) % 3;
            odd = kind < 9 ? R"("n":)" : R"("n":-)";
            for (int i = 0; i < width; ++i) odd.push_back('9');
            break;
        }
    }
    return odd;
}

/// A trace line with a refused member. `head` is the members in front of
/// "args", such as `"name":"read","pid":3`; the line adds `"args"` with
/// "ret":4096 and the member of @ref refused_member.
inline dftracer::utils::dstd::String refused_line(
    int kind, const dftracer::utils::dstd::String& head) {
    return "{" + head + R"(,"args":{"ret":4096,)" + refused_member(kind) + "}}";
}

}  // namespace dftu_utils_test

#endif  // DFTRACER_UTILS_TESTS_REFUSED_LINES_H
