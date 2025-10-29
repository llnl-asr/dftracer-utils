#ifndef DFTRACER_UTILS_CORE_UTILITIES_TAGS_PARALLELIZABLE_H
#define DFTRACER_UTILS_CORE_UTILITIES_TAGS_PARALLELIZABLE_H

namespace dftracer::utils::utilities::tags {

/**
 * @brief Marker tag indicating that a utility is safe for parallel batch
 * execution.
 *
 * This tag indicates that the utility has no shared mutable state and can be
 * safely executed in parallel across multiple threads.
 *
 * Usage:
 * @code
 * class MyUtility : public Utility<Input, Output, tags::Parallelizable> {
 *     Output process(const Input& input) override {
 *         // Thread-safe processing
 *         return result;
 *     }
 * };
 * @endcode
 */
struct Parallelizable {};

}  // namespace dftracer::utils::utilities::tags

#endif  // DFTRACER_UTILS_CORE_UTILITIES_TAGS_PARALLELIZABLE_H
