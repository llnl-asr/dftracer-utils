#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_FILE_COMPRESSOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_FILE_COMPRESSOR_UTILITY_H

#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/fileio/binary_file_reader_utility.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace dftracer::utils::utilities::fileio {

/**
 * @brief Input for file compression workflow.
 */
struct FileCompressionUtilityInput {
    std::string input_path;   ///< Input file path
    std::string output_path;  ///< Output .gz file path (empty = auto-generate)
    int compression_level;    ///< libdeflate level (0-12)
    /// Uncompressed bytes per gzip member; the output is multi-member so it can
    /// be inflated/indexed in parallel.
    std::size_t member_size;

    /**
     * @brief Create input with auto-generated output path.
     */
    static FileCompressionUtilityInput from_file(
        const std::string& input_path, int compression_level = 6,
        std::size_t member_size = constants::indexer::DEFAULT_CHECKPOINT_SIZE) {
        return FileCompressionUtilityInput{
            input_path,
            input_path + ".gz",  // Auto-generate output path
            compression_level, member_size};
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
     * @brief Set the uncompressed bytes per gzip member.
     */
    FileCompressionUtilityInput& with_member_size(std::size_t size) {
        member_size = size;
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
    std::string input_path;       ///< Original input file path
    std::string output_path;      ///< Compressed output file path
    std::size_t original_size;    ///< Original file size (bytes)
    std::size_t compressed_size;  ///< Compressed file size (bytes)

    /**
     * @brief Get compression ratio (compressed / original).
     */
    double compression_ratio() const {
        if (original_size == 0) return 0.0;
        return static_cast<double>(compressed_size) /
               static_cast<double>(original_size);
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
class FileCompressorUtility {
   public:
    /**
     * @brief Compress a file using streaming gzip compression.
     *
     * @param input Compression configuration
     * @return Compression payload, or an error on failure.
     */
    coro::CoroTask<Result<FileCompressionUtilityOutput>> operator()(
        const FileCompressionUtilityInput& input) const {
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

            int level =
                input.compression_level < 0 ? 6 : input.compression_level;
            const std::size_t member_size =
                input.member_size > 0
                    ? input.member_size
                    : constants::indexer::DEFAULT_CHECKPOINT_SIZE;

            GzipWriterOptions opts;
            opts.member_size = member_size;
            opts.level = level;
            auto writer =
                unwrap(co_await GzipLineWriter::open(input.output_path, opts));

            std::string carry;
            auto gen = fileio::read_binary_file(input.input_path, member_size);
            while (auto chunk = co_await gen.next()) {
                std::string_view block(chunk->as<char>(), chunk->size());
                const auto nl = block.rfind('\n');
                if (nl == std::string_view::npos) {
                    carry.append(block);
                    continue;
                }
                carry.append(block.substr(0, nl + 1));
                unwrap(co_await writer.append(carry));
                carry.assign(block.substr(nl + 1));
            }
            unwrap(co_await writer.close(carry));

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

}  // namespace dftracer::utils::utilities::fileio

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_FILE_COMPRESSOR_UTILITY_H
