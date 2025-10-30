#ifndef DFTRACER_UTILS_UTILITIES_HASH_STD_HASHER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_HASH_STD_HASHER_UTILITY_H

#include <dftracer/utils/utilities/hash/internal/base_hasher_utility.h>

#include <cstddef>
#include <functional>
#include <string_view>

namespace dftracer::utils::utilities::hash {

/**
 * @brief std::hash-based hasher utility.
 *
 * Note: This is not a true streaming hash - it combines hashes of chunks.
 * Use xxHash variants for better quality streaming hashing.
 */
class StdHasherUtility : public internal::BaseHasherUtility {
   private:
    std::size_t accumulator_ = 0;

   public:
    StdHasherUtility() { reset(); }

    ~StdHasherUtility() override = default;

    void reset() override {
        accumulator_ = 0;
        current_hash_ = Hash{0};
    }

    void update(std::string_view data) override {
        accumulator_ ^= std::hash<std::string_view>{}(data) + 0x9e3779b9 +
                        (accumulator_ << 6) + (accumulator_ >> 2);
        current_hash_ = Hash{accumulator_};
    }
};

}  // namespace dftracer::utils::utilities::hash

#endif  // DFTRACER_UTILS_UTILITIES_HASH_STD_HASHER_UTILITY_H
