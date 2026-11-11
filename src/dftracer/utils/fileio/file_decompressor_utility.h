// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_FILEIO_FILE_DECOMPRESSOR_UTILITY_H
#define DFTRACER_UTILS_FILEIO_FILE_DECOMPRESSOR_UTILITY_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/fileio/compress/gzip_member_reader.h>
#include <dftracer/utils/fileio/streaming_file_writer_utility.h>
#include <dftracer/utils/runtime/io/io.h>

namespace dftracer::utils::fileio {

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
        const auto found = dstd::fs::exists(dstd::PathView(input.input_path));
        if (!found.has_value()) co_return failed(input, found.error());
        if (!*found) {
            co_return common::make_error<"Input file does not exist: {}">(
                common::ErrorCode::NotFound, input.input_path);
        }

        const auto compressed =
            dstd::fs::file_size(dstd::PathView(input.input_path));
        if (!compressed) co_return failed(input, compressed.error());

        auto opened = co_await runtime::io::open_file(input.input_path, {});
        if (!opened) {
            co_return common::make_error<"Cannot open file: {}">(
                common::ErrorCode::Io, input.input_path);
        }
        const dstd::File fd_file = std::move(*opened);

        const auto st = fd_file.stat();
        if (!st) {
            co_return common::make_error<"Cannot stat file: {}">(
                common::ErrorCode::Io, input.input_path);
        }

        auto written =
            co_await write_plain(input, fd_file.native_handle(), st->size);
        if (!written) co_return failed(input, written.error());

        const auto decompressed =
            dstd::fs::file_size(dstd::PathView(input.output_path));
        if (!decompressed) co_return failed(input, decompressed.error());

        FileDecompressionUtilityOutput payload;
        payload.with_paths(input.input_path, input.output_path)
            .with_sizes(static_cast<std::size_t>(*compressed),
                        static_cast<std::size_t>(*decompressed));
        co_return payload;
    }

   private:
    /// Removes the partial output and reports the cause as a Compression error.
    static dstd::Unexpected<dstd::Error> failed(
        const FileDecompressionUtilityInput& input, const dstd::Error& cause) {
        dstd::fs::remove(dstd::PathView(input.output_path)).discard();
        return common::make_error<"Decompression failed: {}">(
            common::ErrorCode::Compression, cause.message());
    }

    static coro::CoroTask<dstd::Result<void>> write_plain(
        const FileDecompressionUtilityInput& input, int fd,
        std::uint64_t size) {
        auto opened = fileio::StreamingFileWriterUtility::open(
            dstd::PathView(input.output_path));
        if (!opened) co_return opened.error();
        auto writer = std::move(*opened);

        auto gen = fileio::compress::decode_gzip_members(fd, size);
        for (;;) {
            auto chunk = co_await gen.next();
            if (!chunk) break;
            if (!*chunk) co_return chunk->error();
            auto wrote = co_await writer.process(
                dstd::ByteView((*chunk)->data(), (*chunk)->size()));
            if (!wrote) co_return wrote.error();
        }
        writer.close();
        co_return dstd::Result<void>{};
    }
};

}  // namespace dftracer::utils::fileio

#endif  // DFTRACER_UTILS_FILEIO_FILE_DECOMPRESSOR_UTILITY_H
