#ifndef DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_STREAMING_DECOMPRESSOR_H
#define DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_STREAMING_DECOMPRESSOR_H

#include <dftracer/utils/utilities/compression/zlib/shared.h>
#include <dftracer/utils/utilities/io/types/types.h>
#include <zlib.h>

#include <cstring>
#include <stdexcept>

namespace dftracer::utils::utilities::compression::zlib {

using io::CompressedData;
using io::RawData;

/**
 * @brief Manual streaming decompressor that works chunk-by-chunk.
 *
 * This class provides manual control over decompression for advanced use cases.
 * For most cases, consider using the in-memory Decompressor utility instead.
 *
 * Usage:
 * @code
 * StreamingDecompressor decompressor;
 *
 * // Decompress chunks one by one
 * for (const auto& compressed_chunk : compressed_chunks) {
 *     std::vector<RawData> raw_chunks =
 * decompressor.decompress_chunk(compressed_chunk); for (const auto& raw :
 * raw_chunks) {
 *         // Process decompressed data
 *     }
 * }
 * @endcode
 */
class StreamingDecompressorUtility
    : public utilities::Utility<CompressedData, std::vector<RawData>> {
   private:
    z_stream stream_;
    bool initialized_ = false;
    DecompressionFormat format_;
    std::size_t total_in_ = 0;
    std::size_t total_out_ = 0;

    static constexpr std::size_t OUTPUT_BUFFER_SIZE = 64 * 1024;
    std::vector<unsigned char> output_buffer_;

   public:
    explicit StreamingDecompressorUtility(
        DecompressionFormat format = DecompressionFormat::AUTO)
        : format_(format), output_buffer_(OUTPUT_BUFFER_SIZE) {}

    ~StreamingDecompressorUtility() {
        if (initialized_) {
            inflateEnd(&stream_);
        }
    }

    // Non-copyable
    StreamingDecompressorUtility(const StreamingDecompressorUtility&) = delete;
    StreamingDecompressorUtility& operator=(
        const StreamingDecompressorUtility&) = delete;

    /**
     * @brief Decompress a single chunk, yielding output chunks.
     *
     * @param chunk Compressed input chunk
     * @return Vector of decompressed output chunks
     */
    std::vector<RawData> process(const CompressedData& chunk) {
        if (!initialized_) {
            initialize();
        }

        if (chunk.empty()) {
            return {};
        }

        std::vector<RawData> output_chunks;

        stream_.avail_in = static_cast<uInt>(chunk.size());
        stream_.next_in = const_cast<Bytef*>(chunk.data.data());

        do {
            stream_.avail_out = static_cast<uInt>(output_buffer_.size());
            stream_.next_out = output_buffer_.data();

            int ret = inflate(&stream_, Z_NO_FLUSH);

            if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR ||
                ret == Z_MEM_ERROR) {
                throw std::runtime_error("Inflate error: corrupted data");
            }

            std::size_t decompressed_size =
                output_buffer_.size() - stream_.avail_out;
            if (decompressed_size > 0) {
                total_out_ += decompressed_size;

                std::vector<unsigned char> decompressed_data(
                    output_buffer_.begin(),
                    output_buffer_.begin() + decompressed_size);

                output_chunks.push_back(RawData{std::move(decompressed_data)});
            }

            if (ret == Z_STREAM_END) {
                break;
            }

        } while (stream_.avail_out == 0);

        total_in_ += chunk.size();
        return output_chunks;
    }

    std::size_t total_bytes_in() const { return total_in_; }
    std::size_t total_bytes_out() const { return total_out_; }

   private:
    void initialize() {
        std::memset(&stream_, 0, sizeof(stream_));

        int ret = inflateInit2(
            &stream_, static_cast<int>(format_));  // Use format enum value

        if (ret != Z_OK) {
            throw std::runtime_error("Failed to initialize inflate");
        }

        initialized_ = true;
    }
};

}  // namespace dftracer::utils::utilities::compression::zlib

#endif  // DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_STREAMING_DECOMPRESSOR_H
