// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_CONST_STRING_H
#define DFTRACER_UTILS_DSTD_CONST_STRING_H

#include <dftracer/utils/dstd/string.h>

#include <cstddef>
#include <string_view>

namespace dftracer::utils::dstd {

/**
 * @brief Compile-time string buffer for consteval string concatenation.
 *
 *
 * @tparam MaxLen Maximum capacity of the buffer (default 512).
 */
template <std::size_t MaxLen = 512>
struct ConstString {
    char data[MaxLen]{};
    std::size_t len = 0;

    consteval ConstString() = default;

    consteval ConstString(StringView sv) : len(sv.size()) {
        for (std::size_t i = 0; i < len; ++i) data[i] = sv[i];
    }

    consteval ConstString& append(StringView sv) {
        for (std::size_t i = 0; i < sv.size(); ++i) data[len++] = sv[i];
        return *this;
    }

    constexpr StringView view() const { return {data, len}; }
    constexpr operator StringView() const { return {data, len}; }
};

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_CONST_STRING_H
