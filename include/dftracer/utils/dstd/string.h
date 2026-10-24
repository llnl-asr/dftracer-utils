#ifndef DFTRACER_UTILS_DSTD_STRING_H
#define DFTRACER_UTILS_DSTD_STRING_H

#include <dftracer/utils/memory/allocator.h>

#include <string>
#include <string_view>

namespace dftracer::utils::dstd {

using String =
    std::basic_string<char, std::char_traits<char>, memory::Allocator<char>>;
using StringView = std::string_view;

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_STRING_H
