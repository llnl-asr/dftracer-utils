// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_YAML_SCALAR_H
#define DFTRACER_UTILS_YAML_SCALAR_H

#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>

namespace dftracer::utils::yaml {

/// True for the text of a plain scalar that reads as null: empty, `~`, `null`,
/// `Null` and `NULL`.
inline bool is_null_text(dstd::StringView s) {
    if (s.empty()) return true;
    if (s.size() == 1) return s[0] == '~';
    return s == "null" || s == "Null" || s == "NULL";
}

/// All lower case, or one upper case letter and then lower case, or all upper
/// case: the spellings of a bool.
inline bool is_flexible_case(dstd::StringView s) {
    if (s.empty()) return true;
    bool lower = true;
    for (char c : s) lower = lower && dstd::is_ascii_lower(c);
    if (lower) return true;
    if (!dstd::is_ascii_upper(s[0])) return false;
    bool rest_lower = true;
    bool rest_upper = true;
    for (std::size_t i = 1; i < s.size(); ++i) {
        rest_lower = rest_lower && dstd::is_ascii_lower(s[i]);
        rest_upper = rest_upper && dstd::is_ascii_upper(s[i]);
    }
    return rest_lower || rest_upper;
}

/// A string spelled y, n, yes, no, true, false, on or off, in lower case,
/// upper case or capitalized.
inline dstd::Optional<bool> parse_bool(dstd::StringView s) {
    constexpr std::size_t LONGEST = 5;
    if (s.empty() || s.size() > LONGEST || !is_flexible_case(s))
        return dstd::nullopt;
    char lower[LONGEST];
    for (std::size_t i = 0; i < s.size(); ++i)
        lower[i] = dstd::to_ascii_lower(s[i]);
    const dstd::StringView l(lower, s.size());
    switch (s.size()) {
        case 1:
            if (l == "y") return true;
            if (l == "n") return false;
            break;
        case 2:
            if (l == "on") return true;
            if (l == "no") return false;
            break;
        case 3:
            if (l == "yes") return true;
            if (l == "off") return false;
            break;
        case 4:
            if (l == "true") return true;
            break;
        default:
            if (l == "false") return false;
            break;
    }
    return dstd::nullopt;
}

}  // namespace dftracer::utils::yaml

#endif  // DFTRACER_UTILS_YAML_SCALAR_H
