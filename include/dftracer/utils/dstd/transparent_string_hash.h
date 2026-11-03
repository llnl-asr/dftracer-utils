// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_TRANSPARENT_STRING_HASH_H
#define DFTRACER_UTILS_DSTD_TRANSPARENT_STRING_HASH_H

#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vendor/unordered_dense.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace dftracer::utils::dstd {

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
using StringViewMap = HashMap<String, V>;

/// Keyed by std::string_view rather than an owned String. The caller MUST
/// ensure every key outlives the map (e.g. views into an interned pool); a
/// transient view will dangle. Unlike StringViewMap it stores no key copies.
template <typename V>
using InternedStringViewMap = HashMap<std::string_view, V>;

using StringViewSet = HashSet<String>;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_TRANSPARENT_STRING_HASH_H
