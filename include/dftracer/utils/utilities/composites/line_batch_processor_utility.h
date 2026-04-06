#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_LINE_BATCH_PROCESSOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_LINE_BATCH_PROCESSOR_UTILITY_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utilities/utilities.h>
#include <dftracer/utils/utilities/composites/types.h>
#include <dftracer/utils/utilities/fileio/lines/line_types.h>
#include <dftracer/utils/utilities/fileio/lines/streaming_line_reader.h>

#include <functional>
#include <optional>
#include <vector>

namespace dftracer::utils::utilities::composites {

using LineBatchProcessUtilityInput = fileio::lines::LineReadInput;
template <typename LineOutput>
using LineBatchProcessUtilityOutput = std::vector<LineOutput>;

/**
 * @brief Workflow for processing lines from a file with streaming iteration.
 *
 * This workflow:
 * 1. Uses StreamingLineReader for lazy line iteration
 * 2. Applies a processing function to each line
 * 3. Collects results (optional - can filter out nullopt)
 *
 * Template Parameters:
 * - LineOutput: Type returned by the line processor function
 *
 * Usage:
 * @code
 * auto processor = [](const Line& line) -> std::optional<MyData> {
 *     // Process line, return std::nullopt to skip
 *     if (should_process(line)) {
 *         return MyData{...};
 *     }
 *     return std::nullopt;
 * };
 *
 * LineBatchProcessor<MyData> workflow(processor);
 * auto results = workflow.process(LineBatchInput{"/path/to/file.gz",
 * "/path/to/.dftindex"});
 * @endcode
 */
template <typename LineOutput>
class LineBatchProcessorUtility
    : public utilities::Utility<LineBatchProcessUtilityInput,
                                LineBatchProcessUtilityOutput<LineOutput>> {
   public:
    using LineProcessorFn =
        std::function<std::optional<LineOutput>(const fileio::lines::Line&)>;

   private:
    LineProcessorFn processor_;

   public:
    /**
     * @brief Construct processor with a line processing function.
     *
     * @param processor Function that processes a single line
     */
    explicit LineBatchProcessorUtility(LineProcessorFn processor)
        : processor_(std::move(processor)) {}

    /**
     * @brief Process lines from a file using streaming iteration.
     *
     * @param input Line batch configuration
     * @return Vector of processed line results
     */
    coro::CoroTask<LineBatchProcessUtilityOutput<LineOutput>> process(
        const LineBatchProcessUtilityInput& input) override {
        LineBatchProcessUtilityOutput<LineOutput> results;

        auto gen = [&]() {
            if (!input.index_path.empty()) {
                auto iter_config =
                    fileio::lines::sources::IndexedFileLineIteratorConfig()
                        .with_file(input.file_path, input.index_path);
                if (input.start_line > 0 && input.end_line > 0) {
                    iter_config.with_line_range(input.start_line,
                                                input.end_line);
                }
                return fileio::lines::StreamingLineReader::read_indexed_async(
                    iter_config);
            } else {
                return fileio::lines::StreamingLineReader::read_plain_async(
                    input.file_path, input.start_line, input.end_line);
            }
        }();

        while (auto line_opt = co_await gen.next()) {
            auto result = processor_(*line_opt);
            if (result.has_value()) {
                results.push_back(std::move(result.value()));
            }
        }

        co_return results;
    }
};

using SimpleLineBatchProcessUtilityInput = fileio::lines::LineReadInput;
template <typename LineOutput>
using SimpleLineBatchProcessUtilityOutput = std::vector<LineOutput>;

/**
 * @brief Simplified line batch processor that always processes all lines.
 *
 * Use this when you want to process every line without filtering.
 */
template <typename LineOutput>
class SimpleLineBatchProcessorUtility
    : public utilities::Utility<
          SimpleLineBatchProcessUtilityInput,
          SimpleLineBatchProcessUtilityOutput<LineOutput>> {
   public:
    using SimpleLineProcessorFn =
        std::function<LineOutput(const fileio::lines::Line&)>;

   private:
    SimpleLineProcessorFn processor_;

   public:
    explicit SimpleLineBatchProcessorUtility(SimpleLineProcessorFn processor)
        : processor_(std::move(processor)) {}

    coro::CoroTask<SimpleLineBatchProcessUtilityOutput<LineOutput>> process(
        const SimpleLineBatchProcessUtilityInput& input) override {
        SimpleLineBatchProcessUtilityOutput<LineOutput> results;

        auto gen = [&]() {
            if (!input.index_path.empty()) {
                auto iter_config =
                    fileio::lines::sources::IndexedFileLineIteratorConfig()
                        .with_file(input.file_path, input.index_path);
                if (input.start_line > 0 && input.end_line > 0) {
                    iter_config.with_line_range(input.start_line,
                                                input.end_line);
                }
                return fileio::lines::StreamingLineReader::read_indexed_async(
                    iter_config);
            } else {
                return fileio::lines::StreamingLineReader::read_plain_async(
                    input.file_path, input.start_line, input.end_line);
            }
        }();

        while (auto line_opt = co_await gen.next()) {
            results.push_back(processor_(*line_opt));
        }

        co_return results;
    }
};

}  // namespace dftracer::utils::utilities::composites

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_LINE_BATCH_PROCESSOR_UTILITY_H
