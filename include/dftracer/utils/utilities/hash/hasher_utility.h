#ifndef DFTRACER_UTILS_UTILITIES_HASH_HASHER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_HASH_HASHER_UTILITY_H

#include <dftracer/utils/utilities/hash/base_hasher.h>
#include <dftracer/utils/utilities/hash/shared.h>
#include <dftracer/utils/utilities/hash/std_hasher.h>
#include <dftracer/utils/utilities/hash/xxh3_hasher.h>
#include <dftracer/utils/utilities/hash/xxh64_hasher.h>

#include <memory>
#include <string_view>

namespace dftracer::utils::utilities::hash {

/**
 * @brief Unified hasher utility that can use different algorithms.
 *
 * This utility provides a single interface that can switch between different
 * hash algorithms (XXH3, XXH64, std::hash) using a factory pattern.
 *
 * Usage:
 * @code
 * auto hasher =
 * std::make_shared<HasherUtility>(HashAlgorithm::XXH3_64);
 * hasher->reset();
 *
 * hasher->process("chunk1");
 * hasher->process("chunk2");
 * Hash final = hasher->get_hash();  // Get final hash after last process
 *
 * // Or use update() directly for raw data
 * hasher->reset();
 * Hash result = hasher->update("raw data");
 * @endcode
 */
class HasherUtility : public BaseHasherUtility {
   private:
    std::unique_ptr<BaseHasherUtility> impl_;
    HashAlgorithm algorithm_;

   public:
    explicit HasherUtility(HashAlgorithm algo = HashAlgorithm::XXH3_64)
        : algorithm_(algo) {
        create_impl();
        // impl constructors already initialize their state
    }

    ~HasherUtility() override = default;

    // Bring base class overloads into scope
    using BaseHasherUtility::process;
    using BaseHasherUtility::update;

    /**
     * @brief Change the hash algorithm.
     * This will reset the current state.
     */
    void set_algorithm(HashAlgorithm algo) {
        algorithm_ = algo;
        create_impl();
    }

    HashAlgorithm get_algorithm() const { return algorithm_; }

    void reset() override {
        if (!impl_) {
            throw std::runtime_error("impl_ is null in reset()!");
        }
        impl_->reset();
        current_hash_ = impl_->get_hash();
    }

    Hash get_hash() const override { return impl_->get_hash(); }

    void update(std::string_view data) override {
        impl_->update(data);
        current_hash_ = impl_->get_hash();
    }

    // Override process to delegate properly
    Hash process(const std::string& input) override {
        update(input);
        return get_hash();
    }

   private:
    void create_impl() {
        switch (algorithm_) {
            case HashAlgorithm::XXH3_64:
                impl_ = std::make_unique<XXH3HasherUtility>();
                break;
            case HashAlgorithm::XXH64:
                impl_ = std::make_unique<XXH64HasherUtility>();
                break;
            case HashAlgorithm::STD:
                impl_ = std::make_unique<StdHasherUtility>();
                break;
            default:
                impl_ = std::make_unique<XXH3HasherUtility>();
                break;
        }
    }
};

}  // namespace dftracer::utils::utilities::hash

#endif  // DFTRACER_UTILS_UTILITIES_HASH_HASHER_UTILITY_H
