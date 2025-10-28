#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_FILE_COMPRESSOR_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_FILE_COMPRESSOR_H

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/utilities/tags/parallelizable.h>
#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_compressor.h>
#include <dftracer/utils/utilities/io/streaming_file_reader.h>
#include <dftracer/utils/utilities/io/streaming_file_writer.h>

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
 * @brief Output from file compression workflow.
 */
struct FileCompressionUtilityOutput {
    std::string input_path;       // Original input file path
    std::string output_path;      // Compressed output file path
    bool success;                 // Compression succeeded?
    std::size_t original_size;    // Original file size (bytes)
    std::size_t compressed_size;  // Compressed file size (bytes)
    std::string error_message;    // Error message if failed

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
 * Tagged with Parallelizable - safe for parallel batch processing.
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
 * FileCompressionUtilityInput& input, TaskContext& ctx) { return
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
                                FileCompressionUtilityOutput,
                                utilities::tags::Parallelizable> {
   public:
    FileCompressorUtility() = default;
    ~FileCompressorUtility() override = default;

    /**
     * @brief Compress a file using streaming gzip compression.
     *
     * @param input Compression configuration
     * @return Compression result with statistics
     */
    FileCompressionUtilityOutput process(
        const FileCompressionUtilityInput& input) override {
        FileCompressionUtilityOutput result{
            input.input_path,
            input.output_path,
            false,  // success
            0,      // original_size
            0,      // compressed_size
            ""      // error_message
        };

        try {
            // Validate input file exists
            if (!fs::exists(input.input_path)) {
                result.error_message =
                    "Input file does not exist: " + input.input_path;
                return result;
            }

            // Get original file size
            result.original_size = fs::file_size(input.input_path);

            // Step 1: Create streaming reader
            auto reader = std::make_shared<io::StreamingFileReaderUtility>();

            // Step 2: Create manual streaming compressor with specified format
            compression::zlib::ManualStreamingCompressorUtility compressor(
                input.compression_level, input.format);

            // Step 3: Create streaming writer
            io::StreamingFileWriterUtility writer(input.output_path);

            // Step 4: Read and compress chunks
            io::StreamReadInput read_input{input.input_path, input.chunk_size};
            io::ChunkRange chunks = reader->process(read_input);

            for (const auto& chunk : chunks) {
                auto compressed_chunks =
                    compressor.process(io::RawData{chunk.data});
                for (const auto& compressed : compressed_chunks) {
                    io::RawData raw_chunk{compressed.data};
                    writer.process(raw_chunk);
                }
            }

            // Step 5: Finalize compression and write remaining data
            auto final_chunks = compressor.finalize();
            for (const auto& compressed : final_chunks) {
                io::RawData raw_chunk{compressed.data};
                writer.process(raw_chunk);
            }

            // Step 6: Close writer
            writer.close();

            // Get final compressed size
            result.compressed_size = fs::file_size(input.output_path);
            result.success = true;

        } catch (const std::exception& e) {
            result.error_message =
                std::string("Compression failed: ") + e.what();

            // Clean up partial output file on error
            if (fs::exists(input.output_path)) {
                try {
                    fs::remove(input.output_path);
                } catch (...) {
                    // Ignore cleanup errors
                }
            }
        }

        return result;
    }
};

}  // namespace dftracer::utils::utilities::composites

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_FILE_COMPRESSOR_H
