#ifndef DFTRACER_UTILS_CORE_UTILITIES_UTILITIES_H
#define DFTRACER_UTILS_CORE_UTILITIES_UTILITIES_H

/**
 * @file utilities.h
 * @brief Main convenience header for the utilities system.
 *
 * Include this header to get access to the complete utilities framework:
 * - Utility base class
 * - All utility tags
 * - UtilityAdapter for pipeline integration
 * - SFINAE detection traits (in detail namespace)
 *
 * Usage:
 * @code
 * #include <dftracer/utils/core/utilities/utilities.h>
 *
 * using namespace dftracer::utils::utilities;
 *
 * // Define a utility
 * class MyUtility : public Utility<Input, Output> {
 *     Output process(const Input& input) override {
 *         return result;
 *     }
 * };
 *
 * // Use it
 * auto utility = std::make_shared<MyUtility>();
 * Pipeline pipeline;
 * auto result = use(utility).emit_on(pipeline);
 * @endcode
 */

// Core utility components
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/core/utilities/utility_adapter.h>
#include <dftracer/utils/core/utilities/utility_executor.h>
#include <dftracer/utils/core/utilities/utility_traits.h>

// All utility tags
#include <dftracer/utils/core/utilities/tags/tags.h>

// Utility behaviors
#include <dftracer/utils/core/utilities/behaviors/behaviors.h>

#endif  // DFTRACER_UTILS_CORE_UTILITIES_UTILITIES_H
