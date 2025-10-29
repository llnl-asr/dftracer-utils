#ifndef DFTRACER_UTILS_UTILITIES_HASH_XXH64_HASHER_H
#define DFTRACER_UTILS_UTILITIES_HASH_XXH64_HASHER_H

#include <dftracer/utils/utilities/hash/base_hasher.h>
#include <xxhash.h>

#include <stdexcept>
#include <string_view>

namespace dftracer::utils::utilities::hash {

/**
 * @brief XXH64 hasher utility.
 */
class XXH64HasherUtility : public BaseHasherUtility {
   private:
    XXH64_state_t* state_ = nullptr;

   public:
    XXH64HasherUtility() { reset(); }

    ~XXH64HasherUtility() override {
        if (state_) {
            XXH64_freeState(state_);
        }
    }

    void reset() override {
        if (state_) {
            XXH64_freeState(state_);
        }
        state_ = XXH64_createState();
        if (!state_) {
            throw std::runtime_error("Failed to create XXH64 state");
        }
        XXH64_reset(state_, 0);
        current_hash_ = Hash{0};
    }

    void update(std::string_view data) override {
        if (!state_) {
            throw std::runtime_error("XXH64Hasher not initialized");
        }
        XXH64_update(state_, data.data(), data.size());
        // Update current hash with digest
        current_hash_ = Hash{static_cast<std::size_t>(XXH64_digest(state_))};
    }
};

}  // namespace dftracer::utils::utilities::hash

#endif  // DFTRACER_UTILS_UTILITIES_HASH_XXH64_HASHER_H
