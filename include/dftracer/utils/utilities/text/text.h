#ifndef DFTRACER_UTILS_UTILITIES_TEXT_H
#define DFTRACER_UTILS_UTILITIES_TEXT_H

/**
 * @file text.h
 * @brief Convenience header for text component utilities.
 *
 * This header provides composable utilities for text manipulation:
 * - LineSplitter: Split text into lines
 * - LineFilter: Filter lines based on predicate
 * - LinesFilter: Batch filter multiple lines
 * - TextHasher: Compute hash of text
 * - LineHasher: Compute hash of a line
 *
 * Usage:
 * @code
 * #include <dftracer/utils/utilities/text/text.h>
 *
 * using namespace dftracer::utils::utilities::text;
 *
 * // Split text into lines
 * auto splitter = std::make_shared<LineSplitter>();
 * Lines lines = splitter->process(Text{"Line 1\nLine 2\nLine 3"});
 *
 * // Filter lines
 * auto filter = std::make_shared<LinesFilter>([](const Line& line) {
 *     return line.content.find("ERROR") != std::string::npos;
 * });
 * Lines errors = filter->process(lines);
 *
 * // Hash text
 * auto hasher = std::make_shared<TextHasher>();
 * Hash hash = hasher->process(Text{"Some data"});
 * @endcode
 */

#include <dftracer/utils/utilities/text/line_filter.h>
#include <dftracer/utils/utilities/text/line_splitter.h>
#include <dftracer/utils/utilities/text/shared.h>
#include <dftracer/utils/utilities/text/text_hasher.h>

#endif  // DFTRACER_UTILS_UTILITIES_TEXT_H
