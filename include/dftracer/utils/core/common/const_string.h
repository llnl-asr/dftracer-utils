#ifndef DFTRACER_UTILS_CORE_COMMON_CONST_STRING_H
#define DFTRACER_UTILS_CORE_COMMON_CONST_STRING_H

#include <cstddef>
#include <string_view>

namespace dftracer::utils {

/**
 * @brief Compile-time string buffer for consteval string concatenation.
 *
 * Enables building type signatures and display names entirely at compile time.
 * The result lives in .rodata (zero runtime allocation).
 *
 * @tparam MaxLen Maximum capacity of the buffer (default 512).
 */
template <std::size_t MaxLen = 512>
struct ConstString {
    char data[MaxLen]{};
    std::size_t len = 0;

    consteval ConstString() = default;

    consteval ConstString(std::string_view sv) : len(sv.size()) {
        for (std::size_t i = 0; i < len; ++i) data[i] = sv[i];
    }

    consteval ConstString& append(std::string_view sv) {
        for (std::size_t i = 0; i < sv.size(); ++i) data[len++] = sv[i];
        return *this;
    }

    constexpr std::string_view view() const { return {data, len}; }
    constexpr operator std::string_view() const { return {data, len}; }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_CONST_STRING_H
