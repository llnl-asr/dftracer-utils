// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_GZIP_BYTE_STREAM_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_GZIP_BYTE_STREAM_H

#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/utilities/reader/internal/streams/gzip_stream.h>

#include <cstddef>

namespace dftracer::utils::utilities::reader::internal {

class GzipByteStream : public GzipStream {
   private:
    std::size_t buffer_size_;

   public:
    explicit GzipByteStream(std::size_t buffer_size = 0)
        : GzipStream(buffer_size > 0 ? buffer_size : DEFAULT_BUFFER_SIZE),
          buffer_size_(buffer_size > 0 ? buffer_size : DEFAULT_BUFFER_SIZE) {}

    using GzipStream::read_async;

    void initialize(
        const dstd::String &gz_path, std::size_t start_bytes,
        std::size_t end_bytes,
        dftracer::utils::index::gzip::CheckpointIndexer &indexer) override {
        GzipStream::initialize(gz_path, start_bytes, end_bytes, indexer);
        current_position_ = start_bytes;
    }

    coro::CoroTask<dstd::Result<void>> on_initialized() override {
        if (is_finished_) co_return dstd::Result<void>{};
        const std::size_t anchor = seek_anchor_offset();
        if (start_bytes_ > anchor) {
            co_await skip(start_bytes_);
        }
        co_return dstd::Result<void>{};
    }

    coro::CoroTask<dstd::Result<dstd::Span<const char>>> read_async() override {
        auto initialized = co_await ensure_initialized();
        if (!initialized.has_value()) co_return initialized.error();
        if (!decompression_initialized_) {
            co_return reader_error(
                ReaderErrorType::InitializationError,
                "Streaming session not properly initialized");
        }

        if (is_at_target_end()) {
            is_finished_ = true;
            co_return dstd::Span<const char>{};
        }

        size_t max_read = target_end_bytes_ - current_position_;
        size_t read_size = std::min(buffer_size_, max_read);

        size_t bytes_read;
        DFTRACER_UTILS_LOG_DEBUG(
            "GzipByteStream::read (zero-copy) - about to read: read_size={}, "
            "current_position_={}",
            read_size, current_position_);

        bool result = co_await inflater_.read(
            fd_.native_handle(), file_offset_,
            reinterpret_cast<unsigned char *>(buffer_.data()), read_size,
            bytes_read);

        DFTRACER_UTILS_LOG_DEBUG(
            "GzipByteStream::read (zero-copy) - read result: result={}, "
            "bytes_read={}",
            result, bytes_read);

        // A decode failure must not pass for clean EOF (which would silently
        // drop the rest of the file).
        if (!result) {
            co_return reader_error<ReaderErrorType::CompressionError,
                                   "gzip decode failed mid-stream at "
                                   "position {}">(current_position_);
        }
        if (bytes_read == 0) {
            is_finished_ = true;
            co_return dstd::Span<const char>{};
        }

        current_position_ += bytes_read;

        DFTRACER_UTILS_LOG_DEBUG(
            "Streamed (zero-copy) {} bytes (position: {} / {})", bytes_read,
            current_position_, target_end_bytes_);

        co_return dstd::Span<const char>(buffer_.data(), bytes_read);
    }
};

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_GZIP_BYTE_STREAM_H
