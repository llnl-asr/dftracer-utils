#ifndef DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_STREAMING_COMPRESSOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_STREAMING_COMPRESSOR_UTILITY_H

#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/utilities/compression/zlib/types.h>
#include <zlib.h>

#include <cstring>
#include <stdexcept>
#include <vector>

namespace dftracer::utils::utilities::compression::zlib {

/**
 * @brief Manual streaming compressor that yields zero-copy ByteView chunks.
 *
 * Each call to compress() returns an AsyncGenerator that yields ByteView
 * references into the internal output buffer. Call finalize_stream() after
 * all input has been compressed to flush remaining data.
 *
 * Usage:
 * @code
 * ManualStreamingCompressorUtility compressor;
 * auto gen = read_binary_file(path)
 *     .flat_map([&](ByteView chunk) { return compressor.compress(chunk); });
 * while (auto view = co_await gen.next()) {
 *     co_await writer.process(*view);
 * }
 * auto fin = compressor.finalize_stream();
 * while (auto view = co_await fin.next()) {
 *     co_await writer.process(*view);
 * }
 * @endcode
 */
class ManualStreamingCompressorUtility {
   private:
    z_stream stream_;
    bool initialized_ = false;
    bool finalized_ = false;
    int compression_level_;
    CompressionFormat format_;
    std::size_t total_in_ = 0;
    std::size_t total_out_ = 0;

    static constexpr std::size_t OUTPUT_BUFFER_SIZE = 64 * 1024;
    std::vector<unsigned char> output_buffer_;

   public:
    explicit ManualStreamingCompressorUtility(
        int compression_level = Z_DEFAULT_COMPRESSION,
        CompressionFormat format = CompressionFormat::GZIP)
        : compression_level_(compression_level),
          format_(format),
          output_buffer_(OUTPUT_BUFFER_SIZE) {}

    ~ManualStreamingCompressorUtility() {
        if (!finalized_ && initialized_) {
            stream_.avail_in = 0;
            stream_.next_in = nullptr;
            do {
                stream_.avail_out = static_cast<uInt>(output_buffer_.size());
                stream_.next_out = output_buffer_.data();
                deflate(&stream_, Z_FINISH);
            } while (stream_.avail_out == 0);
        }
        if (initialized_) {
            deflateEnd(&stream_);
        }
    }

    ManualStreamingCompressorUtility(const ManualStreamingCompressorUtility&) =
        delete;
    ManualStreamingCompressorUtility& operator=(
        const ManualStreamingCompressorUtility&) = delete;

    /**
     * @brief Compress input bytes, yielding zero-copy views into internal
     * buffer.
     */
    coro::AsyncGenerator<ByteView> compress(ByteView input) {
        if (!initialized_) {
            initialize();
        }

        if (input.empty()) {
            co_return;
        }

        stream_.avail_in = static_cast<uInt>(input.size());
        stream_.next_in = const_cast<Bytef*>(input.as<unsigned char>());

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
                co_yield ByteView(output_buffer_.data(), compressed_size);
            }
        }

        total_in_ += input.size();
    }

    /**
     * @brief Finalize compression, yielding remaining compressed data.
     *
     * Must be called after all input has been compressed.
     */
    coro::AsyncGenerator<ByteView> finalize_stream() {
        if (finalized_) {
            co_return;
        }

        if (!initialized_) {
            initialize();
        }

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
                co_yield ByteView(output_buffer_.data(), compressed_size);
            }
        } while (ret == Z_OK);

        if (ret != Z_STREAM_END) {
            throw std::runtime_error("Failed to finalize compression");
        }

        finalized_ = true;
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

        int ret =
            deflateInit2(&stream_, compression_level_, Z_DEFLATED,
                         static_cast<int>(format_), 8, Z_DEFAULT_STRATEGY);

        if (ret != Z_OK) {
            throw std::runtime_error("Failed to initialize deflate");
        }

        initialized_ = true;
    }
};

}  // namespace dftracer::utils::utilities::compression::zlib

#endif  // DFTRACER_UTILS_UTILITIES_COMPRESSION_ZLIB_STREAMING_COMPRESSOR_UTILITY_H
