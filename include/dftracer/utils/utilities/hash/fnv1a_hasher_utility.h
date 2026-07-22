#ifndef DFTRACER_UTILS_UTILITIES_HASH_FNV1A_HASHER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_HASH_FNV1A_HASHER_UTILITY_H

#include <dftracer/utils/core/common/hash/fnv1a.h>
#include <dftracer/utils/utilities/hash/internal/base_hasher_utility.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dftracer::utils::utilities::hash {

// The primitives live in core so non-utilities code can share them.
using dftracer::utils::hash::fnv1a_hash;
using dftracer::utils::hash::fnv1a_mix;
using dftracer::utils::hash::FNV1A_OFFSET_BASIS;
using dftracer::utils::hash::FNV1A_PRIME;

// Incremental hash builder for combining multiple values
struct Fnv1aHashBuilder {
    std::uint64_t state = FNV1A_OFFSET_BASIS;

    void update(const void* data, std::size_t len) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < len; ++i) {
            state ^= bytes[i];
            state *= FNV1A_PRIME;
        }
    }

    void update(std::string_view data) { update(data.data(), data.size()); }

    template <typename T>
    void update_value(const T& val) {
        update(&val, sizeof(val));
    }

    std::uint64_t finish() const { return state; }
    std::uint32_t finish32() const {
        // XOR-fold 64-bit to 32-bit
        return static_cast<std::uint32_t>(state ^ (state >> 32));
    }
};

/**
 * @brief FNV-1a 64-bit streaming hasher utility.
 *
 * Processes data byte-by-byte, producing consistent results
 * regardless of how input is chunked:
 *   update("Hello"); update("World") == update("HelloWorld")
 */
class Fnv1aHasherUtility : public internal::BaseHasherUtility {
   private:
    std::uint64_t state_ = FNV1A_OFFSET_BASIS;

   public:
    Fnv1aHasherUtility() { reset(); }

    ~Fnv1aHasherUtility() override = default;

    void reset() override {
        state_ = FNV1A_OFFSET_BASIS;
        current_hash_ = Hash{0};
    }

    void update(std::string_view data) override {
        for (unsigned char c : data) {
            state_ ^= c;
            state_ *= FNV1A_PRIME;
        }
        current_hash_ = Hash{static_cast<std::size_t>(state_)};
    }
};

}  // namespace dftracer::utils::utilities::hash

#endif  // DFTRACER_UTILS_UTILITIES_HASH_FNV1A_HASHER_UTILITY_H
