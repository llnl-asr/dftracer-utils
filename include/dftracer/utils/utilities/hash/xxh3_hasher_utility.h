#ifndef DFTRACER_UTILS_UTILITIES_HASH_XXH3_HASHER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_HASH_XXH3_HASHER_UTILITY_H

#include <dftracer/utils/utilities/hash/internal/base_hasher_utility.h>
#include <xxhash.h>

#include <stdexcept>
#include <string_view>

namespace dftracer::utils::utilities::hash {

/**
 * @brief XXH3 64-bit hasher utility.
 */
class XXH3HasherUtility : public internal::BaseHasherUtility {
   private:
    XXH3_state_t* state_ = nullptr;

   public:
    XXH3HasherUtility() { reset(); }

    ~XXH3HasherUtility() override {
        if (state_) {
            XXH3_freeState(state_);
        }
    }

    void reset() override {
        if (state_) {
            XXH3_freeState(state_);
        }
        state_ = XXH3_createState();
        if (!state_) {
            throw std::runtime_error("Failed to create XXH3 state");
        }
        XXH3_64bits_reset_withSeed(state_, 0);
        current_hash_ = Hash{0};
    }

    void update(std::string_view data) override {
        if (!state_) {
            throw std::runtime_error("XXH3Hasher not initialized");
        }
        XXH3_64bits_update(state_, data.data(), data.size());
        current_hash_ =
            Hash{static_cast<std::size_t>(XXH3_64bits_digest(state_))};
    }
};

}  // namespace dftracer::utils::utilities::hash

#endif  // DFTRACER_UTILS_UTILITIES_HASH_XXH3_HASHER_UTILITY_H
