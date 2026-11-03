// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DSTD_STRING_H
#define DFTRACER_UTILS_DSTD_STRING_H

#include <dftracer/utils/memory/allocator.h>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace dftracer::utils::dstd {

using String =
    std::basic_string<char, std::char_traits<char>, memory::Allocator<char>>;
using StringView = std::string_view;

}  // namespace dftracer::utils::dstd

template <>
struct std::hash<dftracer::utils::dstd::String> {
    std::size_t operator()(
        const dftracer::utils::dstd::String& s) const noexcept {
        return std::hash<dftracer::utils::dstd::StringView>{}(s);
    }
};

#endif  // DFTRACER_UTILS_DSTD_STRING_H
