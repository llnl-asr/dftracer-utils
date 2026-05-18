#ifndef DFTRACER_UTILS_CORE_COMMON_TRANSPARENT_STRING_HASH_H
#define DFTRACER_UTILS_CORE_COMMON_TRANSPARENT_STRING_HASH_H

#include <ankerl/unordered_dense.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace dftracer::utils {

struct TransparentStringHash {
    using is_transparent = void;
    using is_avalanching = void;
    std::size_t operator()(std::string_view sv) const noexcept {
        return ankerl::unordered_dense::hash<std::string_view>{}(sv);
    }
    std::size_t operator()(const std::string& s) const noexcept {
        return ankerl::unordered_dense::hash<std::string_view>{}(s);
    }
    std::size_t operator()(const char* s) const noexcept {
        return ankerl::unordered_dense::hash<std::string_view>{}(s);
    }
};

struct TransparentStringEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return a == b;
    }
};

template <typename V>
using StringViewMap =
    ankerl::unordered_dense::map<std::string, V, TransparentStringHash,
                                 TransparentStringEqual>;

using StringViewSet =
    ankerl::unordered_dense::set<std::string, TransparentStringHash,
                                 TransparentStringEqual>;

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_TRANSPARENT_STRING_HASH_H
