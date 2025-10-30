#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_FILE_MERGER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_FILE_MERGER_UTILITY_H

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/utilities/utilities.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/utilities/composites/dft/event_id_extractor_utility.h>
#include <dftracer/utils/utilities/composites/dft/index_builder_utility.h>
#include <dftracer/utils/utilities/composites/file_compressor_utility.h>
#include <dftracer/utils/utilities/composites/line_batch_processor_utility.h>
#include <dftracer/utils/utilities/io/lines/streaming_line_reader.h>
#include <dftracer/utils/utilities/io/streaming_file_writer_utility.h>

#include <atomic>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites {

/**
 * @brief Validated JSON event for merging
 */
struct ValidatedEvent {
    std::string content;  // The validated JSON content
    std::size_t line_number;
};

/**
 * @brief Input for file merge processing
 */
struct FileMergeValidatorUtilityInput {
    std::string file_path;
    std::string index_path;
    std::string output_path;  // Where to write validated events
    std::size_t checkpoint_size{constants::indexer::DEFAULT_CHECKPOINT_SIZE};
    bool force_rebuild{false};

    static FileMergeValidatorUtilityInput from_file(const std::string& path) {
        FileMergeValidatorUtilityInput input;
        input.file_path = path;
        return input;
    }

    FileMergeValidatorUtilityInput& with_index(const std::string& idx_path) {
        index_path = idx_path;
        return *this;
    }

    FileMergeValidatorUtilityInput& with_output(const std::string& out_path) {
        output_path = out_path;
        return *this;
    }

    FileMergeValidatorUtilityInput& with_checkpoint_size(std::size_t size) {
        checkpoint_size = size;
        return *this;
    }

    FileMergeValidatorUtilityInput& with_force_rebuild(bool force) {
        force_rebuild = force;
        return *this;
    }
};

/**
 * @brief Result of processing a single file for merging
 */
struct FileMergeValidatorUtilityOutput {
    std::string file_path;
    std::string output_path;
    bool success{false};
    std::size_t lines_processed{0};
    std::size_t valid_events{0};
    std::size_t total_lines{0};  // Total lines in the input file
};

/**
 * @brief Utility for processing and validating lines from a file for merging
 *
 * This utility:
 * 1. Handles both compressed (.pfw.gz) and plain (.pfw) files
 * 2. Builds or loads indexes for compressed files using IndexBuilderUtility
 * 3. Uses LineBatchProcessor to process lines
 * 4. Validates JSON events and writes to output using StreamingFileWriter
 */
class FileMergeValidatorUtility
    : public utilities::Utility<FileMergeValidatorUtilityInput,
                                FileMergeValidatorUtilityOutput> {
   private:
    static std::atomic<int> file_counter_;

   public:
    FileMergeValidatorUtilityOutput process(
        const FileMergeValidatorUtilityInput& input) override;

    static int get_next_counter() { return file_counter_.fetch_add(1); }
};

/**
 * @brief Input for file merger utility
 */
struct FileMergerUtilityInput {
    std::vector<FileMergeValidatorUtilityOutput> file_results;
    std::string output_file;
    bool compress{false};

    static FileMergerUtilityInput from_results(
        const std::vector<FileMergeValidatorUtilityOutput>& results) {
        FileMergerUtilityInput input;
        input.file_results = results;
        return input;
    }

    FileMergerUtilityInput& with_output(const std::string& path) {
        output_file = path;
        return *this;
    }

    FileMergerUtilityInput& with_compression(bool enable) {
        compress = enable;
        return *this;
    }
};

/**
 * @brief Output from file merger utility
 */
struct FileMergerUtilityOutput {
    bool success{false};
    std::string output_path;
    std::size_t total_events{0};
    std::size_t files_combined{0};
    std::vector<utilities::composites::dft::EventId>
        collected_events;  // Events collected during merge
};

/**
 * @brief Utility to combine temp files into final output
 *
 * This utility:
 * 1. Combines multiple temporary files into a single JSON array
 * 2. Handles proper JSON formatting
 * 3. Optionally compresses the output
 * 4. Cleans up temporary files
 */
class FileMergerUtility : public utilities::Utility<FileMergerUtilityInput,
                                                    FileMergerUtilityOutput> {
   private:
    bool compress_output_file(const std::string& file_path);

   public:
    FileMergerUtilityOutput process(
        const FileMergerUtilityInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_FILE_MERGER_UTILITY_H
