#ifndef DFTRACER_UTILS_DSTD_HASHER_H
#define DFTRACER_UTILS_DSTD_HASHER_H

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/dstd/hash/splitmix64.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <type_traits>

namespace dftracer::utils::dstd {

/// The default hasher of HashMap and HashSet; std::hash is never the default.
/// Every specialization avalanches, so ankerl uses the value as it is.
template <class T, class = void>
struct Hash;

/// One hasher for String, std::string_view and const char*, so a table keyed
/// by any of them finds by the others without building a key.
struct StringHash {
    using is_transparent = void;
    using is_avalanching = void;

    // wyhash through ankerl: FNV-1a in dstd/hash/ reads one byte per step.
    std::size_t operator()(std::string_view s) const noexcept {
        return ankerl::unordered_dense::hash<std::string_view>{}(s);
    }
};

template <>
struct Hash<String> : StringHash {};

template <>
struct Hash<std::string_view> : StringHash {};

template <>
struct Hash<const char*> : StringHash {};

/// Compares String, std::string_view and const char* by their text, as
/// StringHash hashes them; std::equal_to<> compares two const char* by address.
struct StringEqual {
    using is_transparent = void;

    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return a == b;
    }
};

/// The default equality of HashMap and HashSet for key type @p T.
template <class T>
using Equal = std::conditional_t<std::is_same_v<T, String> ||
                                     std::is_same_v<T, std::string_view> ||
                                     std::is_same_v<T, const char*>,
                                 StringEqual, std::equal_to<>>;

template <class T>
struct Hash<T, std::enable_if_t<std::is_integral_v<T>>> {
    using is_avalanching = void;

    std::size_t operator()(T value) const noexcept {
        return static_cast<std::size_t>(
            hash::splitmix64(static_cast<std::uint64_t>(value)));
    }
};

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_HASHER_H
