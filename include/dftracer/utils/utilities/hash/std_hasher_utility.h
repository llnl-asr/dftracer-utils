#ifndef DFTRACER_UTILS_UTILITIES_HASH_STD_HASHER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_HASH_STD_HASHER_UTILITY_H

#include <dftracer/utils/utilities/hash/internal/base_hasher_utility.h>

#include <cstddef>
#include <string_view>

namespace dftracer::utils::utilities::hash {

/**
 * @brief std::hash-based hasher utility.
 *
 * Combines hashes of chunks using the boost hash_combine technique.
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
        dftracer::utils::hash_combine_value(accumulator_, data);
        current_hash_ = Hash{accumulator_};
    }
};

}  // namespace dftracer::utils::utilities::hash

#endif  // DFTRACER_UTILS_UTILITIES_HASH_STD_HASHER_UTILITY_H
