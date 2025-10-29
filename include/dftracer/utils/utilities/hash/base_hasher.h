#ifndef DFTRACER_UTILS_UTILITIES_HASH_BASE_HASHER_H
#define DFTRACER_UTILS_UTILITIES_HASH_BASE_HASHER_H

#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/hash/shared.h>

#include <string>
#include <string_view>
#include <type_traits>

namespace dftracer::utils::utilities::hash {

/**
 * @brief Abstract base class for hash utilities.
 *
 * Provides common interface for incremental hashing utilities.
 * Each concrete implementation wraps a specific hash algorithm.
 *
 * Note: NOT tagged with Parallelizable because it maintains mutable state.
 */
class BaseHasherUtility : public utilities::Utility<std::string, Hash> {
   protected:
    Hash current_hash_{0};

   public:
    virtual ~BaseHasherUtility() = default;

    /**
     * @brief Reset/initialize the hash state.
     * Must be called before first process() or update().
     */
    virtual void reset() = 0;

    /**
     * @brief Update the hash with new data.
     *
     * The hash is computed incrementally and stored in current_hash_.
     * After the last update(), current_hash_ contains the final hash.
     *
     * @param data String view of data to hash
     */
    virtual void update(std::string_view data) = 0;

    /**
     * @brief Update the hash with binary data from a POD type (overload).
     *
     * This allows hashing of structs, integers, and other trivial types
     * by treating them as raw bytes.
     *
     * @tparam T Type to hash (must be trivially copyable)
     * @param value Value to hash
     */
    template <typename T>
    typename std::enable_if<std::is_trivially_copyable<T>::value, void>::type
    update(const T& value) {
        update(
            std::string_view(reinterpret_cast<const char*>(&value), sizeof(T)));
    }

    /**
     * @brief Get the current hash value.
     *
     * This returns the hash computed so far. After the last update(),
     * this is the final hash value.
     *
     * @return Current hash value
     */
    virtual Hash get_hash() const { return current_hash_; }

    /**
     * @brief Process a string chunk (calls update internally).
     *
     * @param input String chunk to add to the hash
     * @return Current hash (updated after processing this chunk)
     */
    Hash process(const std::string& input) override {
        update(input);
        return current_hash_;
    }

    /**
     * @brief Process binary data from a POD type (overload).
     *
     * @tparam T Type to hash (must be trivially copyable)
     * @param value Value to hash
     * @return Current hash (updated after processing this value)
     */
    template <typename T>
    typename std::enable_if<std::is_trivially_copyable<T>::value, Hash>::type
    process(const T& value) {
        update(value);
        return current_hash_;
    }

    /**
     * @brief Process multiple values at once (variadic).
     *
     * Hashes each argument in order. All arguments must be trivially copyable.
     *
     * @tparam Args Types to hash (must be trivially copyable)
     * @param args Values to hash
     * @return Current hash (updated after processing all values)
     */
    template <typename... Args>
    Hash process(const Args&... args) {
        // Fold expression to call update on each argument
        (update(args), ...);
        return current_hash_;
    }
};

}  // namespace dftracer::utils::utilities::hash

#endif  // DFTRACER_UTILS_UTILITIES_HASH_BASE_HASHER_H
