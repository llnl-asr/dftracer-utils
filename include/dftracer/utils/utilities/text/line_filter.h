#ifndef DFTRACER_UTILS_UTILITIES_TEXT_LINE_FILTER_H
#define DFTRACER_UTILS_UTILITIES_TEXT_LINE_FILTER_H

#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/text/shared.h>

#include <functional>
#include <optional>

namespace dftracer::utils::utilities::text {

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
class LineFilterUtility
    : public utilities::Utility<FilterableLine, std::optional<Line>> {
   public:
    LineFilterUtility() = default;
    ~LineFilter() override = default;

    /**
     * @brief Filter a line based on predicate.
     *
     * @param input Line with predicate function
     * @return Optional line (has_value if predicate returns true)
     */
    std::optional<Line> process(const FilterableLine& input) override {
        if (!input.predicate) {
            // No predicate = pass through
            return input.line;
        }

        if (input.predicate(input.line)) {
            return input.line;
        }

        return std::nullopt;
    }
};

/**
 * @brief Utility that filters multiple lines based on a predicate.
 *
 * This is a batch version that processes all lines at once.
 */
class MultiLinesFilterUItility
    : public utilities::Utility<Lines, Lines, utilities::tags::Parallelizable> {
   private:
    std::function<bool(const Line&)> predicate_;

   public:
    explicit MultiLinesFilterUtility(std::function<bool(const Line&)> predicate)
        : predicate_(std::move(predicate)) {}

    ~MultiLinesFilterUtility() override = default;

    /**
     * @brief Set the predicate function.
     */
    void set_predicate(std::function<bool(const Line&)> predicate) {
        predicate_ = std::move(predicate);
    }

    /**
     * @brief Filter lines based on predicate.
     *
     * @param input Lines to filter
     * @return Filtered lines (only those passing predicate)
     */
    Lines process(const Lines& input) override {
        if (!predicate_) {
            // No predicate = pass through all
            return input;
        }

        std::vector<Line> filtered;
        filtered.reserve(input.lines.size());

        for (const auto& line : input.lines) {
            if (predicate_(line)) {
                filtered.push_back(line);
            }
        }

        return Lines{std::move(filtered)};
    }
};

}  // namespace dftracer::utils::utilities::text

#endif  // DFTRACER_UTILS_UTILITIES_TEXT_LINE_FILTER_H
