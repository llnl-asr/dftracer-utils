#ifndef DFTRACER_UTILS_UTILITIES_COMPRESSION_GZIP_STREAMING_COMPRESSOR_H
#define DFTRACER_UTILS_UTILITIES_COMPRESSION_GZIP_STREAMING_COMPRESSOR_H

#include <dftracer/utils/core/utilities/utility.h>
#include <dftracer/utils/utilities/compression/gzip/compressed_chunk_iterator.h>
#include <dftracer/utils/utilities/io/chunk_iterator.h>
#include <dftracer/utils/utilities/io/types/types.h>
#include <zlib.h>

#include <cstring>
#include <memory>
#include <stdexcept>

namespace dftracer::utils::utilities::compression::gzip {

using io::ChunkIterator;
using io::ChunkRange;
using io::CompressedData;
using io::RawData;

/**
 * @brief Streaming gzip compressor utility.
 *
 * This utility compresses a ChunkRange (lazy iterator) and returns a
 * lazy CompressedChunkRange.
 *
 * Composable utility pattern:
 * - Input: ChunkRange (from StreamingFileReader)
 * - Output: CompressedChunkRange (lazy compressed chunks)
 *
 * Usage:
 * @code
 * auto reader = std::make_shared<StreamingFileReader>();
 * auto compressor = std::make_shared<StreamingCompressor>();
 * compressor->set_compression_level(9);
 *
 * io::StreamingFileWriter writer("/output.gz");
 *
 * // Get lazy chunk range
 * ChunkRange chunks = reader->process(StreamReadInput{"/large/file.txt"});
 *
 * // Compress lazily
 * CompressedChunkRange compressed = compressor->process(chunks);
 *
 * // Write each compressed chunk as it's produced
 * for (const auto& chunk : compressed) {
 *     writer.write_chunk(RawData{chunk.data});
 * }
 * writer.close();
 * @endcode
 */
class StreamingCompressorUtility
    : public utilities::Utility<ChunkRange, CompressedChunkRange> {
   private:
    int compression_level_ = Z_DEFAULT_COMPRESSION;

   public:
    StreamingCompressorUtility() = default;
    ~StreamingCompressorUtility() override = default;

    void set_compression_level(int level) {
        if (level < 0 || level > 9) {
            if (level != Z_DEFAULT_COMPRESSION) {
                throw std::invalid_argument(
                    "Compression level must be 0-9 or Z_DEFAULT_COMPRESSION");
            }
        }
        compression_level_ = level;
    }

    int get_compression_level() const { return compression_level_; }

    /**
     * @brief Compress chunk range lazily.
     *
     * Returns a CompressedChunkRange that compresses chunks on-the-fly
     * as you iterate through it. Only one chunk in memory at a time!
     *
     * @param input ChunkRange from StreamingFileReader
     * @return CompressedChunkRange for lazy iteration
     */
    CompressedChunkRange process(const ChunkRange& input) override {
        return CompressedChunkRange{input.begin(), input.end(),
                                    compression_level_};
    }
};

/**
 * @brief Helper class for manual chunk-by-chunk compression (advanced use).
 *
 * Use this if you need more control over compression. For most cases,
 * use StreamingCompressor utility instead.
 */
class ManualStreamingCompressorUtility
    : public utilities::Utility<RawData, std::vector<CompressedData>> {
   private:
    z_stream stream_;
    bool initialized_ = false;
    bool finalized_ = false;
    int compression_level_;
    std::size_t total_in_ = 0;
    std::size_t total_out_ = 0;

    static constexpr std::size_t OUTPUT_BUFFER_SIZE = 64 * 1024;
    std::vector<unsigned char> output_buffer_;

   public:
    explicit ManualStreamingCompressorUtility(
        int compression_level = Z_DEFAULT_COMPRESSION)
        : compression_level_(compression_level),
          output_buffer_(OUTPUT_BUFFER_SIZE) {}

    ~ManualStreamingCompressorUtility() {
        if (initialized_) {
            deflateEnd(&stream_);
        }
        if (!finalized_) {
            finalize();
        }
    }

    // Non-copyable
    ManualStreamingCompressorUtility(const ManualStreamingCompressorUtility&) =
        delete;
    ManualStreamingCompressorUtility& operator=(
        const ManualStreamingCompressorUtility&) = delete;

    std::vector<CompressedData> process(const RawData& chunk) {
        if (!initialized_) {
            initialize();
        }

        if (chunk.empty()) {
            return {};
        }

        std::vector<CompressedData> output_chunks;

        stream_.avail_in = static_cast<uInt>(chunk.size());
        stream_.next_in = const_cast<Bytef*>(chunk.data.data());

        while (stream_.avail_in > 0) {
            stream_.avail_out = static_cast<uInt>(output_buffer_.size());
            stream_.next_out = output_buffer_.data();

            int ret = deflate(&stream_, Z_NO_FLUSH);
            if (ret == Z_STREAM_ERROR) {
                throw std::runtime_error("Deflate stream error");
            }

            std::size_t compressed_size =
                output_buffer_.size() - stream_.avail_out;
            if (compressed_size > 0) {
                total_out_ += compressed_size;

                std::vector<unsigned char> compressed_data(
                    output_buffer_.begin(),
                    output_buffer_.begin() + compressed_size);

                output_chunks.push_back(
                    CompressedData{std::move(compressed_data), chunk.size()});
            }
        }

        total_in_ += chunk.size();
        return output_chunks;
    }

    /**
     * @brief Finalize compression and return remaining data.
     *
     * Must be called after all chunks have been compressed.
     *
     * @return Vector of final compressed chunks
     */
    std::vector<CompressedData> finalize() {
        if (!initialized_) {
            initialize();
        }

        std::vector<CompressedData> output_chunks;
        int ret;

        do {
            stream_.avail_out = static_cast<uInt>(output_buffer_.size());
            stream_.next_out = output_buffer_.data();

            ret = deflate(&stream_, Z_FINISH);
            if (ret == Z_STREAM_ERROR) {
                throw std::runtime_error(
                    "Deflate stream error during finalization");
            }

            std::size_t compressed_size =
                output_buffer_.size() - stream_.avail_out;
            if (compressed_size > 0) {
                total_out_ += compressed_size;

                std::vector<unsigned char> compressed_data(
                    output_buffer_.begin(),
                    output_buffer_.begin() + compressed_size);

                output_chunks.push_back(
                    CompressedData{std::move(compressed_data), 0});
            }
        } while (ret == Z_OK);

        if (ret != Z_STREAM_END) {
            throw std::runtime_error("Failed to finalize compression");
        }

        finalized_ = true;

        return output_chunks;
    }

    std::size_t total_bytes_in() const { return total_in_; }
    std::size_t total_bytes_out() const { return total_out_; }

    double compression_ratio() const {
        if (total_in_ == 0) return 0.0;
        return static_cast<double>(total_out_) / static_cast<double>(total_in_);
    }

   private:
    void initialize() {
        std::memset(&stream_, 0, sizeof(stream_));

        int ret = deflateInit2(&stream_, compression_level_, Z_DEFLATED,
                               15 + 16,  // gzip format
                               8, Z_DEFAULT_STRATEGY);

        if (ret != Z_OK) {
            throw std::runtime_error("Failed to initialize deflate");
        }

        initialized_ = true;
    }
};

}  // namespace dftracer::utils::utilities::compression::gzip

#endif  // DFTRACER_UTILS_UTILITIES_COMPRESSION_GZIP_STREAMING_COMPRESSOR_H
