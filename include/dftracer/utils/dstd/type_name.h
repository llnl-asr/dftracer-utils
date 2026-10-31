#ifndef DFTRACER_UTILS_DSTD_TYPE_NAME_H
#define DFTRACER_UTILS_DSTD_TYPE_NAME_H

#include <string_view>

namespace dftracer::utils::dstd {

/**
 * @brief Get a demangled type name at compile time.
 */
template <typename T>
consteval std::string_view get_type_name() {
#if defined(__clang__)
    // Format: "std::string_view dftracer::utils::get_type_name() [T = int]"
    std::string_view sv = __PRETTY_FUNCTION__;
    auto start = sv.find("T = ") + 4;
    auto end = sv.rfind(']');
    return sv.substr(start, end - start);
#elif defined(__GNUC__)
    // Format: "consteval std::string_view dftracer::utils::get_type_name()
    //          [with T = int; std::string_view = ...]"
    std::string_view sv = __PRETTY_FUNCTION__;
    auto start = sv.find("T = ") + 4;
    auto end = sv.find(';', start);
    if (end == std::string_view::npos) end = sv.rfind(']');
    return sv.substr(start, end - start);
#elif defined(_MSC_VER)
    // Format varies, but __FUNCSIG__ contains the type
    std::string_view sv = __FUNCSIG__;
    auto start = sv.find("get_type_name<") + 14;
    auto end = sv.rfind(">(void)");
    return sv.substr(start, end - start);
#else
    return "unknown";
#endif
}

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_DSTD_TYPE_NAME_H
