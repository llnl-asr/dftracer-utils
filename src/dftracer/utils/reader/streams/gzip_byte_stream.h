#ifndef DFTRACER_UTILS_READER_STREAMS_GZIP_BYTE_STREAM_H
#define DFTRACER_UTILS_READER_STREAMS_GZIP_BYTE_STREAM_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/span.h>
#include <dftracer/utils/reader/streams/gzip_stream.h>

#include <cstddef>
#include <cstdint>
#include <vector>

class GzipByteStream : public GzipStream {
   private:
    std::size_t buffer_size_;
    std::vector<char> buffer_;
    std::size_t valid_bytes_;
    std::size_t buffer_pos_;  // Current position in buffer for copy-based reads
    static constexpr std::size_t DEFAULT_BUFFER_SIZE = 64 * 1024;  // 64KB

   public:
    explicit GzipByteStream(std::size_t buffer_size = 0)
        : GzipStream(),
          buffer_size_(buffer_size > 0 ? buffer_size : DEFAULT_BUFFER_SIZE),
          buffer_(buffer_size_, 0),  // Initialize all bytes to 0
          valid_bytes_(0),
          buffer_pos_(0) {}

    void initialize(const std::string &gz_path, std::size_t start_bytes,
                    std::size_t end_bytes,
                    dftracer::utils::Indexer &indexer) override {
        DFTRACER_UTILS_LOG_DEBUG(
            "GzipByteStream::initialize - start_bytes=%zu, end_bytes=%zu",
            start_bytes, end_bytes);
        GzipStream::initialize(gz_path, start_bytes, end_bytes, indexer);
        current_position_ = start_bytes;
        size_t current_pos = checkpoint_.uc_offset;
        DFTRACER_UTILS_LOG_DEBUG(
            "GzipByteStream::initialize - checkpoint uc_offset=%zu, "
            "using_checkpoint=%s",
            current_pos, use_checkpoint_ ? "true" : "false");
        if (start_bytes > current_pos) {
            DFTRACER_UTILS_LOG_DEBUG(
                "GzipByteStream::initialize - skipping %zu bytes to reach "
                "start_bytes",
                start_bytes - current_pos);
            skip(start_bytes);
        }
        DFTRACER_UTILS_LOG_DEBUG(
            "GzipByteStream::initialize - completed, current_position_=%zu",
            current_position_);
    }

    span_view<const char> read() override {
        if (!decompression_initialized_) {
            throw ReaderError(ReaderError::INITIALIZATION_ERROR,
                              "Streaming session not properly initialized");
        }

        if (is_at_target_end()) {
            is_finished_ = true;
            return {};
        }

        size_t max_read = target_end_bytes_ - current_position_;
        size_t read_size = std::min(buffer_size_, max_read);

        size_t bytes_read;
        DFTRACER_UTILS_LOG_DEBUG(
            "GzipByteStream::read (zero-copy) - about to read: read_size=%zu, "
            "current_position_=%zu",
            read_size, current_position_);

        bool result = inflater_.read(
            file_handle_, reinterpret_cast<unsigned char *>(buffer_.data()),
            read_size, bytes_read);

        DFTRACER_UTILS_LOG_DEBUG(
            "GzipByteStream::read (zero-copy) - read result: result=%d, "
            "bytes_read=%zu",
            result, bytes_read);

        if (!result || bytes_read == 0) {
            DFTRACER_UTILS_LOG_DEBUG("%s",
                                     "GzipByteStream::read (zero-copy) - "
                                     "marking as finished due to read "
                                     "failure or 0 bytes");
            is_finished_ = true;
            return {};
        }

        current_position_ += bytes_read;

        DFTRACER_UTILS_LOG_DEBUG(
            "Streamed (zero-copy) %zu bytes (position: %zu / %zu)", bytes_read,
            current_position_, target_end_bytes_);

        return dftracer::utils::span_view<const char>(buffer_.data(),
                                                      bytes_read);
    }

    std::size_t read(char *buffer, std::size_t buffer_size) override {
#ifdef __GNUC__
        __builtin_prefetch(buffer, 1, 3);
#endif

        // Check if we have unconsumed data from previous read
        if (buffer_pos_ < valid_bytes_) {
            std::size_t remaining = valid_bytes_ - buffer_pos_;
            std::size_t copy_size = std::min(remaining, buffer_size);
            std::memcpy(buffer, buffer_.data() + buffer_pos_, copy_size);
            buffer_pos_ += copy_size;

            DFTRACER_UTILS_LOG_DEBUG(
                "Copied %zu bytes from existing buffer (pos %zu/%zu)",
                copy_size, buffer_pos_, valid_bytes_);

            return copy_size;
        }

        // Buffer exhausted, get new chunk via zero-copy read
        auto span = read();
        if (span.empty()) {
            return 0;
        }

        // Update our tracking of the buffer state
        valid_bytes_ = span.size();
        buffer_pos_ = 0;

        std::size_t copy_size = std::min(valid_bytes_, buffer_size);
        std::memcpy(buffer, span.data(), copy_size);
        buffer_pos_ = copy_size;

        DFTRACER_UTILS_LOG_DEBUG(
            "Got new chunk via zero-copy, copied %zu bytes (total in buffer: "
            "%zu)",
            copy_size, valid_bytes_);

        return copy_size;
    }
};

#endif  // DFTRACER_UTILS_READER_STREAMS_GZIP_BYTE_STREAM_H
