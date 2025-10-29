#ifndef DFTRACER_UTILS_UTILITIES_HASH_MT_HASHER_H
#define DFTRACER_UTILS_UTILITIES_HASH_MT_HASHER_H

#include <dftracer/utils/utilities/hash/hasher_utility.h>
#include <dftracer/utils/utilities/hash/shared.h>

#include <mutex>
#include <string_view>

namespace dftracer::utils::utilities::hash {

/**
 * @brief Thread-safe version of HasherUtility.
 *
 * Extends HasherUtility with mutex protection, making all operations
 * thread-safe. Supports all the same features (algorithm switching, etc.)
 * with added concurrency safety.
 *
 * Usage:
 * @code
 * // Create thread-safe hasher with default algorithm
 * auto hasher = std::make_shared<MTHasherUtility>();
 *
 * // Or specify algorithm
 * auto hasher =
 * std::make_shared<MTHasherUtility>(HashAlgorithm::XXH3_64);
 *
 * // Safe to use from multiple threads
 * hasher->reset();
 * Hash result = hasher->update("chunk1");  // Thread-safe
 * result = hasher->update("chunk2");  // Thread-safe
 * @endcode
 *
 * Note: While this makes the hasher thread-safe, you still need to coordinate
 * the order of updates across threads if you want deterministic results.
 */
class MTHasherUtility : public HasherUtility {
   private:
    mutable std::mutex mutex_;

   public:
    explicit MTHasherUtility(HashAlgorithm algo = HashAlgorithm::XXH3_64)
        : HasherUtility(algo) {}

    ~MTHasherUtility() override = default;

    // Bring base class overloads into scope
    using HasherUtility::process;
    using HasherUtility::update;

    void set_algorithm(HashAlgorithm algo) {
        std::lock_guard<std::mutex> lock(mutex_);
        HasherUtility::set_algorithm(algo);
    }

    void reset() override {
        std::lock_guard<std::mutex> lock(mutex_);
        HasherUtility::reset();
    }

    void update(std::string_view data) override {
        std::lock_guard<std::mutex> lock(mutex_);
        HasherUtility::update(data);
    }

    Hash get_hash() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return HasherUtility::get_hash();
    }
};

}  // namespace dftracer::utils::utilities::hash

#endif  // DFTRACER_UTILS_UTILITIES_HASH_MT_HASHER_H
