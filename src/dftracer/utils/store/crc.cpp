// SPDX-License-Identifier: MIT
#include <dftracer/utils/store/crc.h>
#include <libdeflate.h>

namespace dftracer::utils::store::engine {

std::uint32_t crc32(std::uint32_t crc, const void* data, std::size_t len) {
    // libdeflate picks its crc32 kernel on first use by writing unsynchronized
    // globals, so the first call runs once, under a function-local static.
    [[maybe_unused]] static const bool RESOLVED = [] {
        const unsigned char byte = 0;
        libdeflate_crc32(0, &byte, 0);
        return true;
    }();
    return libdeflate_crc32(crc, data, len);
}

}  // namespace dftracer::utils::store::engine
