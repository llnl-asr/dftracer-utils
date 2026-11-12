// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/bits.h>

#include <cstring>

namespace dftracer::utils::dstd::bits {

std::size_t popcount_bytes(const std::uint8_t* data, std::size_t n) {
    std::size_t count = 0;
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        std::uint64_t w;
        std::memcpy(&w, data + i, sizeof(w));
        count += static_cast<std::size_t>(popcount_u64(w));
    }
    for (; i < n; ++i) count += static_cast<std::size_t>(popcount_u64(data[i]));
    return count;
}

}  // namespace dftracer::utils::dstd::bits
