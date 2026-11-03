// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_TEXT_H
#define DFTRACER_UTILS_UTILITIES_TEXT_H

/**
 * @file text.h
 * @brief Convenience header for text component utilities.
 *
 * This header provides composable utilities for text manipulation:
 * - LineFilter: Filter a single line based on a predicate
 * - LinesFilter: Batch filter multiple lines
 *
 * Usage:
 * @code
 * #include <dftracer/utils/utilities/text/text.h>
 *
 * using namespace dftracer::utils::utilities::text;
 *
 * // Filter lines
 * auto filter = dstd::make_shared<LinesFilter>([](const Line& line) {
 *     return line.content.find("ERROR") != dstd::String::npos;
 * });
 * Lines errors = filter->process(lines);
 * @endcode
 */

#include <dftracer/utils/utilities/text/line_filter.h>
#include <dftracer/utils/utilities/text/shared.h>

#endif  // DFTRACER_UTILS_UTILITIES_TEXT_H
