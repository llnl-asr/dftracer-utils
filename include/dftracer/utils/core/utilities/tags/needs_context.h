#ifndef DFTRACER_UTILS_CORE_UTILITIES_TAGS_NEEDS_CONTEXT_H
#define DFTRACER_UTILS_CORE_UTILITIES_TAGS_NEEDS_CONTEXT_H

namespace dftracer::utils::utilities::tags {

/**
 * @brief Marker tag indicating that a utility needs TaskContext for dynamic
 * task emission.
 *
 * Usage:
 * @code
 * class MyUtility : public Utility<Input, Output, tags::NeedsContext> {
 *     Output process(const Input& input, TaskContext& ctx) override {
 *         // Can use ctx.emit() here for dynamic task emission
 *         return result;
 *     }
 * };
 * @endcode
 */
struct NeedsContext {};

}  // namespace dftracer::utils::utilities::tags

#endif  // DFTRACER_UTILS_CORE_UTILITIES_TAGS_NEEDS_CONTEXT_H
