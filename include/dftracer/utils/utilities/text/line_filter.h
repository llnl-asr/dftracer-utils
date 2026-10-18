#ifndef DFTRACER_UTILS_UTILITIES_TEXT_LINE_FILTER_H
#define DFTRACER_UTILS_UTILITIES_TEXT_LINE_FILTER_H

#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/utilities/text/shared.h>

#include <functional>
#include <optional>

namespace dftracer::utils::utilities::text {

/// Import line types for convenience
using fileio::lines::Line;
using fileio::lines::Lines;

/**
 * @brief Utility that filters lines based on a predicate function.
 *
 * This utility takes a FilterableLine (line + predicate) and returns the line
 * only if it passes the predicate. Otherwise, returns empty optional.
 *
 * Features:
 * - Flexible predicate-based filtering
 * - Returns std::optional for composability
 * - Can be used in map/filter pipelines
 * - Can be tagged with Retryable, Monitored behaviors
 *   (Note: Cacheable not recommended due to std::function in input)
 *
 * Usage:
 * @code
 * auto filter = std::make_shared<LineFilter>();
 *
 * auto predicate = [](const Line& line) {
 *     return line.content.find("ERROR") != std::string::npos;
 * };
 *
 * FilterableLine input{Line{"ERROR: Something went wrong"}, predicate};
 * auto result = filter->process(input);
 *
 * if (result.has_value()) {
 *     std::cout << "Matched: " << result->content << "\n";
 * }
 * @endcode
 *
 * Filtering multiple lines:
 * @code
 * auto is_error = [](const Line& line) {
 *     return line.content.find("ERROR") != std::string::npos;
 * };
 *
 * Lines input = get_lines();
 * std::vector<Line> filtered;
 *
 * for (const auto& line : input.lines) {
 *     auto result = filter->process(FilterableLine{line, is_error});
 *     if (result.has_value()) {
 *         filtered.push_back(*result);
 *     }
 * }
 * @endcode
 */
class LineFilterUtility {
   public:
    /**
     * @brief Filter a line based on predicate.
     *
     * @param input Line with predicate function
     * @return Optional line (has_value if predicate returns true)
     */
    coro::CoroTask<std::optional<Line>> operator()(
        const FilterableLine& input) const {
        if (!input.predicate) {
            // No predicate = pass through
            co_return input.line;
        }

        if (input.predicate(input.line)) {
            co_return input.line;
        }

        co_return std::nullopt;
    }
};

}  // namespace dftracer::utils::utilities::text

#endif  // DFTRACER_UTILS_UTILITIES_TEXT_LINE_FILTER_H
