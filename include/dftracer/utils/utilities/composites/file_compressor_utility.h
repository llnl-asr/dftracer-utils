#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_FILE_COMPRESSOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_FILE_COMPRESSOR_UTILITY_H

#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_compressor_utility.h>
#include <dftracer/utils/utilities/fileio/binary_file_reader_utility.h>
#include <dftracer/utils/utilities/fileio/streaming_file_writer_utility.h>

#include <string>

namespace dftracer::utils::utilities::composites {

/**
 * @brief Input for file compression workflow.
 */
struct FileCompressionUtilityInput {
    std::string input_path;   // Input file path
    std::string output_path;  // Output .gz file path (empty = auto-generate)
    int compression_level;  // Compression level (0-9, or Z_DEFAULT_COMPRESSION)
    std::size_t chunk_size;  // Chunk size for streaming (bytes)
    compression::zlib::CompressionFormat format =
        compression::zlib::CompressionFormat::AUTO;

    /**
     * @brief Create input with auto-generated output path.
     */
    static FileCompressionUtilityInput from_file(
        const std::string& input_path,
        int compression_level = Z_DEFAULT_COMPRESSION,
        std::size_t chunk_size = 64 * 1024) {
        return FileCompressionUtilityInput{
            input_path,
            input_path + ".gz",  // Auto-generate output path
            compression_level, chunk_size,
            compression::zlib::CompressionFormat::GZIP  // Default to GZIP
        };
    }

    /**
     * @brief Set output path.
     */
    FileCompressionUtilityInput& with_output(const std::string& path) {
        output_path = path;
        return *this;
    }

    /**
     * @brief Set compression level.
     */
    FileCompressionUtilityInput& with_compression_level(int level) {
        compression_level = level;
        return *this;
    }

    /**
     * @brief Set chunk size.
     */
    FileCompressionUtilityInput& with_chunk_size(std::size_t size) {
        chunk_size = size;
        return *this;
    }

    /**
     * @brief Set compression format.
     */
    FileCompressionUtilityInput& with_format(
        compression::zlib::CompressionFormat fmt) {
        format = fmt;
        return *this;
    }
};

/**
 * @brief Success payload from file compression workflow.
 *
 * Failures are reported via Result<FileCompressionUtilityOutput>, so this
 * struct carries only the successful-result data.
 */
struct FileCompressionUtilityOutput {
    std::string input_path;       // Original input file path
    std::string output_path;      // Compressed output file path
    std::size_t original_size;    // Original file size (bytes)
    std::size_t compressed_size;  // Compressed file size (bytes)

    /**
     * @brief Get compression ratio (compressed / original).
     */
    double compression_ratio() const {
        if (original_size == 0) return 0.0;
        return static_cast<double>(compressed_size) /
               static_cast<double>(original_size);
    }

    /**
     * @brief Get compression percentage (how much space saved).
     */
    double compression_percentage() const {
        return (1.0 - compression_ratio()) * 100.0;
    }
};

/**
 * @brief Workflow for compressing files using streaming gzip compression.
 *
 * This workflow:
 * 1. Reads input file in chunks using StreamingFileReader
 * 2. Compresses each chunk using StreamingCompressor
 * 3. Writes compressed data to .gz file using StreamingFileWriter
 *
 * Usage:
 * @code
 * // Single file compression
 * auto compressor = std::make_shared<FileCompressor>();
 * auto input = FileCompressionInput::from_file("large_file.txt")
 *                  .with_compression_level(9);
 * auto result = compressor->process(input);
 *
 * // Parallel batch compression
 * auto batch_compressor = std::make_shared<
 *     BatchProcessor<FileCompressionUtilityInput,
 * FileCompressionUtilityOutput>>( [compressor](const
 * FileCompressionUtilityInput& input, CoroScope& ctx) { return
 * compressor->process(input);
 *         }
 * );
 *
 * std::vector<FileCompressionInput> files = { ... };
 * auto results = batch_compressor->process(files);
 * @endcode
 */
class FileCompressorUtility
    : public utilities::Utility<FileCompressionUtilityInput,
                                Result<FileCompressionUtilityOutput>> {
   public:
    FileCompressorUtility() = default;
    ~FileCompressorUtility() override = default;

    /**
     * @brief Compress a file using streaming gzip compression.
     *
     * @param input Compression configuration
     * @return Compression payload, or an error on failure.
     */
    coro::CoroTask<Result<FileCompressionUtilityOutput>> process(
        const FileCompressionUtilityInput& input) override {
        std::size_t original_size = 0;
        std::size_t compressed_size = 0;

        try {
            // Validate input file exists
            if (!fs::exists(input.input_path)) {
                co_return make_error(
                    ErrorCode::NOT_FOUND,
                    "Input file does not exist: " + input.input_path);
            }

            // Get original file size
            original_size = fs::file_size(input.input_path);

            compression::zlib::ManualStreamingCompressorUtility compressor(
                input.compression_level, input.format);
            fileio::StreamingFileWriterUtility writer(input.output_path);

            auto gen =
                (fileio::read_binary_file(input.input_path, input.chunk_size) >>
                 [&](ByteView chunk) { return compressor.compress(chunk); }) |
                [&] { return compressor.finalize_stream(); };

            while (auto out = co_await gen.next()) {
                co_await writer.process(*out);
            }

            writer.close();

            // Get final compressed size
            compressed_size = fs::file_size(input.output_path);

        } catch (const std::exception& e) {
            // Clean up partial output file on error
            remove_file_quietly(input.output_path);

            co_return make_error(
                ErrorCode::COMPRESSION,
                std::string("Compression failed: ") + e.what());
        }

        FileCompressionUtilityOutput payload;
        payload.input_path = input.input_path;
        payload.output_path = input.output_path;
        payload.original_size = original_size;
        payload.compressed_size = compressed_size;
        co_return payload;
    }
};

}  // namespace dftracer::utils::utilities::composites

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_FILE_COMPRESSOR_UTILITY_H
