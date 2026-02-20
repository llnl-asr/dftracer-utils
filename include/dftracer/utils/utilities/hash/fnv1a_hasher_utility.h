#ifndef DFTRACER_UTILS_UTILITIES_HASH_FNV1A_HASHER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_HASH_FNV1A_HASHER_UTILITY_H

#include <dftracer/utils/utilities/hash/internal/base_hasher_utility.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dftracer::utils::utilities::hash {

/**
 * @brief FNV-1a 64-bit streaming hasher utility.
 *
 * Processes data byte-by-byte, producing consistent results
 * regardless of how input is chunked:
 *   update("Hello"); update("World") == update("HelloWorld")
 */
class Fnv1aHasherUtility : public internal::BaseHasherUtility {
   private:
    static constexpr std::uint64_t FNV_OFFSET_BASIS = 0xcbf29ce484222325ULL;
    static constexpr std::uint64_t FNV_PRIME = 0x00000100000001B3ULL;

    std::uint64_t state_ = FNV_OFFSET_BASIS;

   public:
    Fnv1aHasherUtility() { reset(); }

    ~Fnv1aHasherUtility() override = default;

    void reset() override {
        state_ = FNV_OFFSET_BASIS;
        current_hash_ = Hash{0};
    }

    void update(std::string_view data) override {
        for (unsigned char c : data) {
            state_ ^= c;
            state_ *= FNV_PRIME;
        }
        current_hash_ = Hash{static_cast<std::size_t>(state_)};
    }
};

}  // namespace dftracer::utils::utilities::hash

#endif  // DFTRACER_UTILS_UTILITIES_HASH_FNV1A_HASHER_UTILITY_H
