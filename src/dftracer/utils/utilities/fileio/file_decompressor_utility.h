// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_FILE_DECOMPRESSOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_FILE_DECOMPRESSOR_UTILITY_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/runtime/io/io.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_member_reader.h>
#include <dftracer/utils/utilities/fileio/streaming_file_writer_utility.h>

namespace dftracer::utils::utilities::fileio {

/**
 * @brief Input for file decompression workflow.
 */
struct FileDecompressionUtilityInput {
    dstd::String input_path;  ///< Input .gz file path
    dstd::String
        output_path;  ///< Output decompressed file path (empty = auto-generate)

    /**
     * @brief Create input with auto-generated output path.
     *
     * Strips .gz extension from input path to generate output path.
     */
    static FileDecompressionUtilityInput from_file(
        dstd::StringView input_path) {
        dstd::String output(input_path);

        // Strip .gz extension if present
        if (output.size() > 3 && output.substr(output.size() - 3) == ".gz") {
            output = output.substr(0, output.size() - 3);
        } else {
            // If no .gz extension, append .decompressed
            output += ".decompressed";
        }

        return FileDecompressionUtilityInput{dstd::String(input_path), output};
    }

    /**
     * @brief Fluent builder: Set output path.
     */
    FileDecompressionUtilityInput& with_output(dstd::StringView path) {
        output_path = dstd::String(path);
        return *this;
    }
};

/**
 * @brief Success payload from file decompression workflow.
 *
 * Failures are reported via dstd::Result<FileDecompressionUtilityOutput>, so
 * this struct carries only the successful-result data.
 */
struct FileDecompressionUtilityOutput {
    dstd::String input_path;        ///< Original .gz input file path
    dstd::String output_path;       ///< Decompressed output file path
    std::size_t compressed_size;    ///< Compressed file size (bytes)
    std::size_t decompressed_size;  ///< Decompressed file size (bytes)

    FileDecompressionUtilityOutput()
        : compressed_size(0), decompressed_size(0) {}

    FileDecompressionUtilityOutput& with_paths(dstd::StringView in_path,
                                               dstd::StringView out_path) {
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
 * auto decompressor = dstd::make_shared<FileDecompressor>();
 * auto input = FileDecompressionInput::from_file("archive.gz");
 * auto result = decompressor->process(input);
 *
 * // Parallel batch decompression
 * auto batch_decompressor = dstd::make_shared<
 *     BatchProcessor<FileDecompressionUtilityInput,
 * FileDecompressionUtilityOutput>>( [decompressor](const
 * FileDecompressionUtilityInput& input, coro::Scope& ctx) { return
 * decompressor->process(input);
 *         }
 * );
 *
 * dstd::Vector<FileDecompressionUtilityInput> files = { ... };
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
    coro::CoroTask<dstd::Result<FileDecompressionUtilityOutput>> operator()(
        const FileDecompressionUtilityInput& input) const {
        std::size_t compressed_size = 0;
        std::size_t decompressed_size = 0;

        try {
            // Validate input file exists
            const auto found =
                dstd::fs::exists(dstd::PathView(input.input_path));
            if (!found) throw common::DFTUtilsException(found.error());
            if (!*found) {
                co_return common::make_error(
                    common::ErrorCode::NotFound,
                    "Input file does not exist: " + input.input_path);
            }

            // Get compressed file size
            const auto compressed =
                dstd::fs::file_size(dstd::PathView(input.input_path));
            if (!compressed)
                throw common::DFTUtilsException(compressed.error());
            compressed_size = static_cast<std::size_t>(*compressed);

            auto opened = co_await runtime::io::open_file(input.input_path, {});
            if (!opened) {
                co_return common::make_error(
                    common::ErrorCode::Io,
                    "Cannot open file: " + input.input_path);
            }
            const dstd::File fd_file = std::move(*opened);
            const int fd = fd_file.native_handle();

            const auto st = fd_file.stat();
            if (!st) {
                co_return common::make_error(
                    common::ErrorCode::Io,
                    "Cannot stat file: " + input.input_path);
            }

            fileio::StreamingFileWriterUtility writer(
                dstd::PathView(input.output_path));
            auto gen = fileio::compress::decode_gzip_members(fd, st->size);
            for (;;) {
                auto chunk = co_await gen.next();
                if (!chunk) break;
                co_await writer.process(
                    dstd::ByteView(chunk->data(), chunk->size()));
            }

            writer.close();

            // Get final decompressed size
            const auto decompressed =
                dstd::fs::file_size(dstd::PathView(input.output_path));
            if (!decompressed) {
                throw common::DFTUtilsException(decompressed.error());
            }
            decompressed_size = static_cast<std::size_t>(*decompressed);

        } catch (const std::exception& e) {
            // Clean up partial output file on error
            dstd::fs::remove(dstd::PathView(input.output_path)).discard();

            co_return common::make_error(
                common::ErrorCode::Compression,
                dstd::String("Decompression failed: ") + e.what());
        }

        FileDecompressionUtilityOutput payload;
        payload.with_paths(input.input_path, input.output_path)
            .with_sizes(compressed_size, decompressed_size);
        co_return payload;
    }
};

}  // namespace dftracer::utils::utilities::fileio

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_FILE_DECOMPRESSOR_UTILITY_H
