#ifndef DFTRACER_UTILS_CORE_COMMON_TRANSPARENT_STRING_HASH_H
#define DFTRACER_UTILS_CORE_COMMON_TRANSPARENT_STRING_HASH_H

#include <cstddef>
#include <functional>
#include <string_view>

namespace dftracer::utils {

/**
 * @brief Transparent hash for std::unordered_map<std::string, ...> that
 * accepts std::string_view lookups without constructing std::string.
 *
 * Usage:
 * @code
 *   std::unordered_map<std::string, int, TransparentStringHash,
 *                      TransparentStringEqual> map;
 *   map[some_string_view];  // no std::string construction for lookup
 * @endcode
 */
struct TransparentStringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
};

struct TransparentStringEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return a == b;
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_TRANSPARENT_STRING_HASH_H
