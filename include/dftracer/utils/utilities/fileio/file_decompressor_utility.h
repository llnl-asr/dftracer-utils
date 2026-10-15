#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_FILE_DECOMPRESSOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_FILE_DECOMPRESSOR_UTILITY_H

#include <dftracer/utils/common/byte_view.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/common/scoped_fd.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_member_reader.h>
#include <dftracer/utils/utilities/fileio/streaming_file_writer_utility.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <string>

namespace dftracer::utils::utilities::fileio {

/**
 * @brief Input for file decompression workflow.
 */
struct FileDecompressionUtilityInput {
    std::string input_path;  ///< Input .gz file path
    std::string
        output_path;  ///< Output decompressed file path (empty = auto-generate)

    /**
     * @brief Create input with auto-generated output path.
     *
     * Strips .gz extension from input path to generate output path.
     */
    static FileDecompressionUtilityInput from_file(
        const std::string& input_path) {
        std::string output = input_path;

        // Strip .gz extension if present
        if (output.size() > 3 && output.substr(output.size() - 3) == ".gz") {
            output = output.substr(0, output.size() - 3);
        } else {
            // If no .gz extension, append .decompressed
            output += ".decompressed";
        }

        return FileDecompressionUtilityInput{input_path, output};
    }

    /**
     * @brief Fluent builder: Set output path.
     */
    FileDecompressionUtilityInput& with_output(const std::string& path) {
        output_path = path;
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
    std::string input_path;         ///< Original .gz input file path
    std::string output_path;        ///< Decompressed output file path
    std::size_t compressed_size;    ///< Compressed file size (bytes)
    std::size_t decompressed_size;  ///< Decompressed file size (bytes)

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
};

/**
 * @brief Workflow for decompressing a multi-member gzip file.
 *
 * Decodes the input a gzip member at a time with libdeflate and writes the
 * decompressed bytes to the output file. Peak memory is one member.
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
class FileDecompressorUtility {
   public:
    /**
     * @brief Decompress a gzip file using streaming decompression.
     *
     * @param input Decompression configuration
     * @return Decompression payload, or an error on failure.
     */
    coro::CoroTask<Result<FileDecompressionUtilityOutput>> operator()(
        const FileDecompressionUtilityInput& input) const {
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

            ssize_t fd_result =
                co_await io::open(input.input_path.c_str(), O_RDONLY);
            if (fd_result < 0) {
                co_return make_error(ErrorCode::IO,
                                     "Cannot open file: " + input.input_path);
            }
            ScopedFd fd(static_cast<int>(fd_result));

            struct stat st;
            if (::fstat(fd.get(), &st) != 0) {
                co_return make_error(ErrorCode::IO,
                                     "Cannot stat file: " + input.input_path);
            }

            fileio::StreamingFileWriterUtility writer(input.output_path);
            auto gen = fileio::compress::decode_gzip_members(
                fd.get(), static_cast<std::uint64_t>(st.st_size));
            while (auto chunk = co_await gen.next()) {
                co_await writer.process(ByteView(chunk->data(), chunk->size()));
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

}  // namespace dftracer::utils::utilities::fileio

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_FILE_DECOMPRESSOR_UTILITY_H
