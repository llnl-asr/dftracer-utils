#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_GZIP_BYTE_STREAM_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_GZIP_BYTE_STREAM_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/reader/internal/streams/gzip_stream.h>

#include <cstddef>
#include <span>
#include <vector>

namespace dftracer::utils::utilities::reader::internal {

class GzipByteStream : public GzipStream {
   private:
    std::size_t buffer_size_;

   public:
    explicit GzipByteStream(std::size_t buffer_size = 0)
        : GzipStream(buffer_size > 0 ? buffer_size : DEFAULT_BUFFER_SIZE),
          buffer_size_(buffer_size > 0 ? buffer_size : DEFAULT_BUFFER_SIZE) {}

    using GzipStream::read_async;

    void initialize(const std::string &gz_path, std::size_t start_bytes,
                    std::size_t end_bytes,
                    dftracer::utils::utilities::indexer::internal::Indexer
                        &indexer) override {
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

    coro::CoroTask<std::span<const char>> read_async() override {
        if (!decompression_initialized_) {
            throw ReaderError(ReaderError::INITIALIZATION_ERROR,
                              "Streaming session not properly initialized");
        }

        if (is_at_target_end()) {
            is_finished_ = true;
            co_return {};
        }

        size_t max_read = target_end_bytes_ - current_position_;
        size_t read_size = std::min(buffer_size_, max_read);

        size_t bytes_read;
        DFTRACER_UTILS_LOG_DEBUG(
            "GzipByteStream::read (zero-copy) - about to read: read_size=%zu, "
            "current_position_=%zu",
            read_size, current_position_);

        bool result = co_await inflater_.read(
            fd_, file_offset_,
            reinterpret_cast<unsigned char *>(buffer_.data()), read_size,
            bytes_read);

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
            co_return {};
        }

        current_position_ += bytes_read;

        DFTRACER_UTILS_LOG_DEBUG(
            "Streamed (zero-copy) %zu bytes (position: %zu / %zu)", bytes_read,
            current_position_, target_end_bytes_);

        co_return std::span<const char>(buffer_.data(), bytes_read);
    }
};

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_GZIP_BYTE_STREAM_H
