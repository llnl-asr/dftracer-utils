#ifndef DFTRACER_UTILS_CORE_COMMON_LITTLE_ENDIAN_H
#define DFTRACER_UTILS_CORE_COMMON_LITTLE_ENDIAN_H

#include <cstdint>

namespace dftracer::utils {

// Little-endian fixed-width byte codec. Callers own the buffer and are
// responsible for bounds; no checks are performed on these hot decode paths.

inline std::uint32_t read_u32_le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

inline void write_u32_le(std::uint8_t* p, std::uint32_t val) {
    if (!p) return;
    p[0] = static_cast<std::uint8_t>(val & 0xFF);
    p[1] = static_cast<std::uint8_t>((val >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>((val >> 16) & 0xFF);
    p[3] = static_cast<std::uint8_t>((val >> 24) & 0xFF);
}

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_LITTLE_ENDIAN_H
