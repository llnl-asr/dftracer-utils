#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_FILE_DECOMPRESSOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_FILE_DECOMPRESSOR_UTILITY_H

#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_decompressor_utility.h>
#include <dftracer/utils/utilities/fileio/binary_file_reader_utility.h>
#include <dftracer/utils/utilities/fileio/streaming_file_writer_utility.h>

#include <string>

namespace dftracer::utils::utilities::composites {

/**
 * @brief Input for file decompression workflow.
 */
struct FileDecompressionUtilityInput {
    std::string input_path;  // Input .gz file path
    std::string
        output_path;  // Output decompressed file path (empty = auto-generate)
    std::size_t chunk_size;  // Chunk size for streaming (bytes)
    // Decompression format (AUTO detects automatically)
    compression::zlib::DecompressionFormat format =
        compression::zlib::DecompressionFormat::AUTO;

    /**
     * @brief Create input with auto-generated output path.
     *
     * Strips .gz extension from input path to generate output path.
     */
    static FileDecompressionUtilityInput from_file(
        const std::string& input_path, std::size_t chunk_size = 64 * 1024) {
        std::string output = input_path;

        // Strip .gz extension if present
        if (output.size() > 3 && output.substr(output.size() - 3) == ".gz") {
            output = output.substr(0, output.size() - 3);
        } else {
            // If no .gz extension, append .decompressed
            output += ".decompressed";
        }

        return FileDecompressionUtilityInput{
            input_path, output, chunk_size,
            compression::zlib::DecompressionFormat::AUTO  // Default to AUTO
                                                          // detection
        };
    }

    /**
     * @brief Fluent builder: Set output path.
     */
    FileDecompressionUtilityInput& with_output(const std::string& path) {
        output_path = path;
        return *this;
    }

    /**
     * @brief Fluent builder: Set chunk size.
     */
    FileDecompressionUtilityInput& with_chunk_size(std::size_t size) {
        chunk_size = size;
        return *this;
    }

    /**
     * @brief Fluent builder: Set decompression format.
     */
    FileDecompressionUtilityInput& with_format(
        compression::zlib::DecompressionFormat fmt) {
        format = fmt;
        return *this;
    }
};

/**
 * @brief Success payload from file decompression workflow.
 *
 * Failures are reported via Result<FileDecompressionUtilityOutput>, so this
 * struct carries only the successful-result data.
 */
struct FileDecompressionUtilityOutput {
    std::string input_path;         // Original .gz input file path
    std::string output_path;        // Decompressed output file path
    std::size_t compressed_size;    // Compressed file size (bytes)
    std::size_t decompressed_size;  // Decompressed file size (bytes)

    FileDecompressionUtilityOutput()
        : compressed_size(0), decompressed_size(0) {}

    FileDecompressionUtilityOutput& with_paths(const std::string& in_path,
                                               const std::string& out_path) {
        input_path = in_path;
        output_path = out_path;
        return *this;
    }

    FileDecompressionUtilityOutput& with_sizes(std::size_t comp_size,
                                               std::size_t decomp_size) {
        compressed_size = comp_size;
        decompressed_size = decomp_size;
        return *this;
    }

    FileDecompressionUtilityOutput& with_compressed_size(std::size_t size) {
        compressed_size = size;
        return *this;
    }

    FileDecompressionUtilityOutput& with_decompressed_size(std::size_t size) {
        decompressed_size = size;
        return *this;
    }

    FileDecompressionUtilityOutput& with_input_path(const std::string& path) {
        input_path = path;
        return *this;
    }

    FileDecompressionUtilityOutput& with_output_path(const std::string& path) {
        output_path = path;
        return *this;
    }

    /**
     * @brief Get compression ratio of the original file.
     */
    double original_compression_ratio() const {
        if (decompressed_size == 0) return 0.0;
        return static_cast<double>(compressed_size) /
               static_cast<double>(decompressed_size);
    }
};

/**
 * @brief Workflow for decompressing gzip files using streaming decompression.
 *
 * This workflow:
 * 1. Reads compressed .gz file in chunks using StreamingFileReader
 * 2. Decompresses each chunk using StreamingDecompressor
 * 3. Writes decompressed data to output file using StreamingFileWriter
 *
 * Usage:
 * @code
 * // Single file decompression
 * auto decompressor = std::make_shared<FileDecompressor>();
 * auto input = FileDecompressionInput::from_file("archive.gz");
 * auto result = decompressor->process(input);
 *
 * // Parallel batch decompression
 * auto batch_decompressor = std::make_shared<
 *     BatchProcessor<FileDecompressionUtilityInput,
 * FileDecompressionUtilityOutput>>( [decompressor](const
 * FileDecompressionUtilityInput& input, CoroScope& ctx) { return
 * decompressor->process(input);
 *         }
 * );
 *
 * std::vector<FileDecompressionUtilityInput> files = { ... };
 * auto results = batch_decompressor->process(files);
 * @endcode
 */
class FileDecompressorUtility
    : public utilities::Utility<FileDecompressionUtilityInput,
                                Result<FileDecompressionUtilityOutput>> {
   public:
    FileDecompressorUtility() = default;
    ~FileDecompressorUtility() override = default;

    /**
     * @brief Decompress a gzip file using streaming decompression.
     *
     * @param input Decompression configuration
     * @return Decompression payload, or an error on failure.
     */
    coro::CoroTask<Result<FileDecompressionUtilityOutput>> process(
        const FileDecompressionUtilityInput& input) override {
        std::size_t compressed_size = 0;
        std::size_t decompressed_size = 0;

        try {
            // Validate input file exists
            if (!fs::exists(input.input_path)) {
                co_return make_error(
                    ErrorCode::NOT_FOUND,
                    "Input file does not exist: " + input.input_path);
            }

            // Get compressed file size
            compressed_size = fs::file_size(input.input_path);

            compression::zlib::StreamingDecompressorUtility decompressor(
                input.format);
            fileio::StreamingFileWriterUtility writer(input.output_path);

            auto gen =
                fileio::read_binary_file(input.input_path, input.chunk_size) >>
                [&](ByteView chunk) { return decompressor.decompress(chunk); };
            while (auto out = co_await gen.next()) {
                co_await writer.process(*out);
            }

            writer.close();

            // Get final decompressed size
            decompressed_size = fs::file_size(input.output_path);

        } catch (const std::exception& e) {
            // Clean up partial output file on error
            remove_file_quietly(input.output_path);

            co_return make_error(
                ErrorCode::COMPRESSION,
                std::string("Decompression failed: ") + e.what());
        }

        FileDecompressionUtilityOutput payload;
        payload.with_paths(input.input_path, input.output_path)
            .with_sizes(compressed_size, decompressed_size);
        co_return payload;
    }
};

}  // namespace dftracer::utils::utilities::composites

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_FILE_DECOMPRESSOR_UTILITY_H
