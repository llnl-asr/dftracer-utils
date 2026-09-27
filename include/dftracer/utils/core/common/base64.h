#ifndef DFTRACER_UTILS_CORE_COMMON_BASE64_H
#define DFTRACER_UTILS_CORE_COMMON_BASE64_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace dftracer::utils {

/// Standard base64 (RFC 4648 alphabet) with `=` padding.
std::string base64_encode(const void* data, std::size_t len);

/// Inverse of base64_encode; nullopt when `sv` is not padded base64.
std::optional<std::string> base64_decode(std::string_view sv);

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_BASE64_H
