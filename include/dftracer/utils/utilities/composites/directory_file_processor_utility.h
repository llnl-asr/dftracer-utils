#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DIRECTORY_FILE_PROCESSOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DIRECTORY_FILE_PROCESSOR_UTILITY_H

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/core/tasks/task_context.h>
#include <dftracer/utils/core/utilities/utilities.h>
#include <dftracer/utils/utilities/composites/types.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites {

/**
 * @brief Generic workflow for scanning a directory and processing files in
 * parallel.
 *
 * This workflow utility:
 * 1. Scans a directory for files matching specified extensions
 * 2. Processes each file in parallel using TaskContext::emit()
 * 3. Aggregates results and waits for completion
 *
 * Template Parameters:
 * - FileOutput: Type returned by the file processor function
 *
 * Usage:
 * @code
 * auto processor = [](TaskContext& ctx, const std::string& path) {
 *     // Process file and return result
 *     return MyFileOutput{...};
 * };
 *
 * DirectoryFileProcessor<MyFileOutput> workflow(processor);
 * auto output = workflow.process(DirectoryProcessInput{"/path/to/dir"});
 * @endcode
 */
template <typename FileOutput>
class DirectoryFileProcessorUtility
    : public utilities::Utility<DirectoryProcessInput,
                                BatchFileProcessOutput<FileOutput>,
                                utilities::tags::NeedsContext> {
   public:
    using FileProcessorFn =
        std::function<FileOutput(TaskContext&, const std::string&)>;

   private:
    FileProcessorFn processor_;
    filesystem::PatternDirectoryScannerUtility scanner_;

   public:
    /**
     * @brief Construct processor with a file processing function.
     *
     * @param processor Function that processes a single file
     */
    explicit DirectoryFileProcessorUtility(FileProcessorFn processor)
        : processor_(std::move(processor)) {}

    /**
     * @brief Process all files in a directory in parallel.
     *
     * @param input Directory configuration
     * @return Aggregated results from all file processing
     */
    BatchFileProcessOutput<FileOutput> process(
        const DirectoryProcessInput& input) override {
        BatchFileProcessOutput<FileOutput> output;

        // Step 1: Use PatternDirectoryScanner to scan and filter
        filesystem::PatternDirectoryScannerUtilityInput pattern_input{
            input.directory_path, input.extensions, input.recursive};
        std::vector<filesystem::FileEntry> matched_entries =
            scanner_.process(pattern_input);

        if (matched_entries.empty()) {
            return output;  // No files found
        }

        // Step 2: Extract file paths
        std::vector<std::string> files;
        files.reserve(matched_entries.size());
        for (const auto& entry : matched_entries) {
            files.push_back(entry.path.string());
        }

        // Step 3: Get TaskContext for parallel execution
        TaskContext& ctx = this->context();

        // Step 4: Submit parallel tasks for each file
        std::vector<std::shared_future<std::any>> futures;
        futures.reserve(files.size());

        for (const auto& file_path : files) {
            // Create task from processor - captures ctx from outer scope
            auto task = make_task(
                [proc = processor_, &ctx](std::string path) -> FileOutput {
                    return proc(ctx, path);
                });

            // Submit task with input
            auto future = ctx.submit_task(task, std::any{file_path});
            futures.push_back(future);
        }

        // Step 5: Wait for all tasks to complete (synchronization point)
        output.results.reserve(files.size());
        for (auto& future : futures) {
            std::any result_any = future.get();
            output.results.push_back(std::any_cast<FileOutput>(result_any));
        }

        // Step 6: Finalize aggregated statistics
        output.finalize();

        return output;
    }
};

}  // namespace dftracer::utils::utilities::composites

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DIRECTORY_FILE_PROCESSOR_UTILITY_H
