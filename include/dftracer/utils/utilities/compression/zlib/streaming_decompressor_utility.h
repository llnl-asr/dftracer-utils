#ifndef DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_STREAMING_DECOMPRESSOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_STREAMING_DECOMPRESSOR_UTILITY_H

#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/utilities/compression/zlib/types.h>
#include <zlib.h>

#include <cstring>
#include <stdexcept>
#include <vector>

namespace dftracer::utils::utilities::compression::zlib {

/**
 * @brief Streaming decompressor that yields zero-copy ByteView chunks.
 *
 * Each call to decompress() returns an AsyncGenerator that yields ByteView
 * references into the internal output buffer. The view is valid until the
 * next co_await gen.next() call. Callers must consume each view before
 * advancing the generator.
 *
 * Usage:
 * @code
 * StreamingDecompressorUtility decompressor;
 * for (const auto& compressed_chunk : chunks) {
 *     auto gen = decompressor.decompress(ByteView(chunk_data, chunk_len));
 *     while (auto view = co_await gen.next()) {
 *         process(view->as<char>(), view->size());
 *     }
 * }
 * @endcode
 */
class StreamingDecompressorUtility {
   private:
    z_stream stream_;
    bool initialized_ = false;
    bool finished_ = false;
    bool between_members_ = false;
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

    StreamingDecompressorUtility(const StreamingDecompressorUtility&) = delete;
    StreamingDecompressorUtility& operator=(
        const StreamingDecompressorUtility&) = delete;

    /**
     * @brief Decompress input bytes, yielding zero-copy views into internal
     * buffer.
     *
     * Each yielded ByteView points into output_buffer_ and is valid only
     * until the next iteration. Handles concatenated gzip members (RFC 1952).
     *
     * @param input Compressed input bytes
     * @return AsyncGenerator yielding ByteView chunks
     */
    coro::AsyncGenerator<ByteView> decompress(ByteView input) {
        if (!initialized_) {
            initialize();
        }

        if (input.empty()) {
            co_return;
        }

        if (finished_) {
            inflateReset2(&stream_, static_cast<int>(format_));
            finished_ = false;
            between_members_ = true;
        }

        stream_.avail_in = static_cast<uInt>(input.size());
        stream_.next_in = const_cast<Bytef*>(input.as<unsigned char>());

        do {
            stream_.avail_out = static_cast<uInt>(output_buffer_.size());
            stream_.next_out = output_buffer_.data();

            int ret = inflate(&stream_, Z_NO_FLUSH);

            if (ret == Z_STREAM_ERROR || ret == Z_MEM_ERROR) {
                throw std::runtime_error("Inflate error: corrupted data");
            }

            if (ret == Z_DATA_ERROR) {
                if (between_members_) {
                    finished_ = true;
                    break;
                }
                throw std::runtime_error("Inflate error: corrupted data");
            }

            std::size_t decompressed_size =
                output_buffer_.size() - stream_.avail_out;
            if (decompressed_size > 0) {
                between_members_ = false;
                total_out_ += decompressed_size;
                co_yield ByteView(output_buffer_.data(), decompressed_size);
            }

            if (ret == Z_STREAM_END) {
                if (stream_.avail_in > 0) {
                    inflateReset2(&stream_, static_cast<int>(format_));
                    between_members_ = true;
                    continue;
                }
                finished_ = true;
                break;
            }

        } while (stream_.avail_out == 0 || stream_.avail_in > 0);

        total_in_ += input.size();
    }

    std::size_t total_bytes_in() const { return total_in_; }
    std::size_t total_bytes_out() const { return total_out_; }

   private:
    void initialize() {
        std::memset(&stream_, 0, sizeof(stream_));

        int ret = inflateInit2(&stream_, static_cast<int>(format_));

        if (ret != Z_OK) {
            throw std::runtime_error("Failed to initialize inflate");
        }

        initialized_ = true;
    }
};

}  // namespace dftracer::utils::utilities::compression::zlib

#endif  // DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_STREAMING_DECOMPRESSOR_UTILITY_H
