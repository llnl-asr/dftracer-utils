#ifndef DFTRACER_UTILS_UTILITIES_HASH_FNV1A_HASHER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_HASH_FNV1A_HASHER_UTILITY_H

#include <dftracer/utils/core/common/hash/fnv1a.h>
#include <dftracer/utils/core/common/hash/hash_combine.h>
#include <dftracer/utils/utilities/hash/types.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <vector>

namespace dftracer::utils::utilities::hash {

/// The primitives live in core so non-utilities code can share them.
using dftracer::utils::hash::fnv1a_hash;
using dftracer::utils::hash::fnv1a_mix;
using dftracer::utils::hash::FNV1A_OFFSET_BASIS;
using dftracer::utils::hash::FNV1A_PRIME;

/// Incremental hash builder for combining multiple values.
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
 * @brief Incremental FNV-1a 64-bit hasher.
 *
 * Chunk-order independent: `update("Hello"); update("World")` yields the same
 * hash as `update("HelloWorld")`. Holds mutable state, so a single instance is
 * not safe to use concurrently.
 */
class Fnv1aHasherUtility {
   public:
    Fnv1aHasherUtility() { reset(); }

    /// Reset the hash state; call before the first update().
    void reset() {
        state_ = FNV1A_OFFSET_BASIS;
        current_hash_ = Hash{0};
    }

    void update(std::string_view data) {
        for (unsigned char c : data) {
            state_ ^= c;
            state_ *= FNV1A_PRIME;
        }
        current_hash_ = Hash{static_cast<std::size_t>(state_)};
    }

    /// Hash a C-string as bytes, not as a pointer address.
    void update(const char* str) { update(std::string_view(str)); }

    /// Hash a trivially-copyable value as its raw bytes. Pointer types are
    /// excluded so a string literal is never hashed as an address.
    template <typename T>
    typename std::enable_if<
        std::is_trivially_copyable<T>::value &&
            !std::is_pointer<typename std::decay<T>::type>::value,
        void>::type
    update(const T& value) {
        update(
            std::string_view(reinterpret_cast<const char*>(&value), sizeof(T)));
    }

    void update(const std::vector<unsigned char>& data) {
        update(std::string_view(reinterpret_cast<const char*>(data.data()),
                                data.size()));
    }

    /// The hash accumulated so far.
    Hash get_hash() const { return current_hash_; }

    /// Update with each argument in order and return the running hash.
    template <typename... Args>
    Hash process(const Args&... args) {
        (update(args), ...);
        return get_hash();
    }

    /// Combine a hash value into a seed (boost-style).
    static void hash_combine(std::size_t& seed, std::size_t value) {
        dftracer::utils::hash_combine(seed, value);
    }

   private:
    std::uint64_t state_ = FNV1A_OFFSET_BASIS;
    Hash current_hash_{0};
};

}  // namespace dftracer::utils::utilities::hash

#endif  // DFTRACER_UTILS_UTILITIES_HASH_FNV1A_HASHER_UTILITY_H
