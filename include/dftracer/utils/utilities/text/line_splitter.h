#ifndef DFTRACER_UTILS_UTILITIES_TEXT_LINE_SPLITTER_H
#define DFTRACER_UTILS_UTILITIES_TEXT_LINE_SPLITTER_H

#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/text/shared.h>

#include <sstream>
#include <string>

namespace dftracer::utils::utilities::text {

// Import line types for convenience
using io::lines::Line;
using io::lines::Lines;

/**
 * @brief Utility that splits text into individual lines.
 *
 * This utility takes multi-line text and splits it into a vector of lines,
 * preserving line numbers. It can be used standalone or composed in pipelines.
 *
 * Features:
 * - Splits on '\n' characters
 * - Assigns line numbers (1-indexed)
 * - Handles empty lines
 * - Can be tagged with Cacheable, Retryable, Monitored behaviors
 *
 * Usage:
 * @code
 * auto splitter = std::make_shared<LineSplitter>();
 *
 * Text input("Line 1\nLine 2\nLine 3");
 * Lines output = splitter->process(input);
 *
 * for (const auto& line : output.lines) {
 *     std::cout << line.line_number << ": " << line.content << "\n";
 * }
 * @endcode
 *
 * With pipeline:
 * @code
 * Pipeline pipeline;
 * auto task = use(splitter).emit_on(pipeline);
 * auto output = SequentialExecutor().execute(pipeline, Text{"data"});
 * auto lines = output.get<Lines>(task.id());
 * @endcode
 */
class LineSplitterUtility
    : public utilities::Utility<Text, Lines, utilities::tags::Parallelizable> {
   public:
    LineSplitterUtility() = default;
    ~LineSplitterUtility() override = default;

    /**
     * @brief Split text into lines.
     *
     * @param input Text to split
     * @return Lines with line numbers
     */
    Lines process(const Text& input) override {
        if (input.empty()) {
            return Lines{};
        }

        // Build string storage first
        std::vector<std::string> string_storage;
        std::istringstream stream(input.content);
        std::string line_content;

        while (std::getline(stream, line_content)) {
            string_storage.push_back(std::move(line_content));
        }

        // Use the Lines constructor that takes ownership and creates views
        return Lines{std::move(string_storage)};
    }
};

}  // namespace dftracer::utils::utilities::text

#endif  // DFTRACER_UTILS_UTILITIES_TEXT_LINE_SPLITTER_H
