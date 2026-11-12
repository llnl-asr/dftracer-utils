// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_TRANSPARENT_STRING_HASH_H
#define DFTRACER_UTILS_DSTD_TRANSPARENT_STRING_HASH_H

#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/hasher.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace dftracer::utils::dstd {

struct TransparentStringHash {
    using is_transparent = void;
    using is_avalanching = void;
    std::size_t operator()(StringView sv) const noexcept {
        return Hash<StringView>{}(sv);
    }
    std::size_t operator()(const String& s) const noexcept {
        return Hash<StringView>{}(s);
    }
    std::size_t operator()(const char* s) const noexcept {
        return Hash<StringView>{}(s);
    }
};

struct TransparentStringEqual {
    using is_transparent = void;
    bool operator()(StringView a, StringView b) const noexcept {
        return a == b;
    }
};

template <typename V>
using StringViewMap = HashMap<String, V>;

/// Keyed by std::string_view rather than an owned String. The caller MUST
/// ensure every key outlives the map (e.g. views into an interned pool); a
/// transient view will dangle. Unlike StringViewMap it stores no key copies.
template <typename V>
using InternedStringViewMap = HashMap<StringView, V>;

using StringViewSet = HashSet<String>;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_TRANSPARENT_STRING_HASH_H
