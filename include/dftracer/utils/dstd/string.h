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
// keep std: defines dstd::StringView and its hash
using StringView = std::string_view;

}  // namespace dftracer::utils::dstd

/// libstdc++ before 13 hashes only a std::string with std::allocator (LWG
/// 3705). The value matches std::hash<std::string> for the same text.
template <>
struct std::hash<dftracer::utils::dstd::String> {
    std::size_t operator()(
        const dftracer::utils::dstd::String& s) const noexcept {
        // keep std: defines dstd::StringView and its hash
        return std::hash<std::string_view>{}(s);
    }
};

#endif  // DFTRACER_UTILS_DSTD_STRING_H
