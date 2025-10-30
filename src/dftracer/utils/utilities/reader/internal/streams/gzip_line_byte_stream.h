#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_GZIP_LINE_BYTE_STREAM_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_GZIP_LINE_BYTE_STREAM_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/common/span.h>
#include <dftracer/utils/utilities/reader/internal/streams/gzip_stream.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dftracer::utils::utilities::reader::internal {

class GzipLineByteStream : public GzipStream {
   private:
    static constexpr std::size_t SEARCH_BUFFER_SIZE = 2048;
    static constexpr std::size_t LINE_SEARCH_LOOKBACK = 512;
    static constexpr std::size_t DEFAULT_BUFFER_SIZE = 64 * 1024;  // 64KB

    std::vector<char> partial_line_buffer_;
    std::size_t actual_start_bytes_;
    std::size_t bytes_returned_;  // Track how many bytes we've returned to user

    // Buffer for zero-copy reads
    std::vector<char> buffer_;
    std::size_t valid_bytes_;
    std::size_t buffer_pos_;  // Current position in buffer for copy-based reads

   public:
    GzipLineByteStream(std::size_t buffer_size = DEFAULT_BUFFER_SIZE)
        : GzipStream(),
          actual_start_bytes_(0),
          bytes_returned_(0),
          buffer_(buffer_size > 0 ? buffer_size : DEFAULT_BUFFER_SIZE, 0),
          valid_bytes_(0),
          buffer_pos_(0) {
        partial_line_buffer_.reserve(1 * 1024 * 1024);
    }

    void initialize(const std::string &gz_path, std::size_t start_bytes,
                    std::size_t end_bytes,
                    dftracer::utils::utilities::indexer::internal::Indexer
                        &indexer) override {
        GzipStream::initialize(gz_path, start_bytes, end_bytes, indexer);
        actual_start_bytes_ = find_line_start(start_bytes);
        current_position_ = actual_start_bytes_;
    }

    std::size_t find_line_start(std::size_t target_start) {
        std::size_t current_pos = checkpoint_.uc_offset;
        std::size_t actual_start = target_start;

        // If target is at the start of the file, no adjustment needed
        if (target_start <= current_pos) {
            return target_start;
        }

        std::size_t search_start = (target_start >= LINE_SEARCH_LOOKBACK)
                                       ? target_start - LINE_SEARCH_LOOKBACK
                                       : current_pos;

        if (search_start > current_pos) {
            skip(search_start);
            current_pos = search_start;
        }

        // Use stack allocation for small search buffer
        alignas(DFTRACER_OPTIMAL_ALIGNMENT) unsigned char
            search_buffer[SEARCH_BUFFER_SIZE];
        std::size_t search_bytes;
        if (inflater_.read(file_handle_, search_buffer,
                           sizeof(search_buffer) - 1, search_bytes)) {
            std::size_t relative_target = target_start - current_pos;
            if (relative_target < search_bytes) {
                // Always use backward search to find line start
                // This ensures we start at the beginning of a complete line
                for (int64_t i = static_cast<int64_t>(relative_target); i >= 0;
                     i--) {
                    if (i == 0 || search_buffer[i - 1] == '\n') {
                        actual_start =
                            current_pos + static_cast<std::size_t>(i);
                        break;
                    }
                }
            }
        }

        restart_compression();
        if (actual_start > checkpoint_.uc_offset) {
            skip(actual_start);
        }

        return actual_start;
    }

    std::size_t read(char *output_buffer,
                     std::size_t output_buffer_size) override {
#ifdef __GNUC__
        __builtin_prefetch(output_buffer, 1, 3);
#endif

        // Check if we have unconsumed data from previous read
        if (buffer_pos_ < valid_bytes_) {
            std::size_t remaining = valid_bytes_ - buffer_pos_;
            std::size_t copy_size = std::min(remaining, output_buffer_size);
            std::memcpy(output_buffer, buffer_.data() + buffer_pos_, copy_size);
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

        std::size_t copy_size = std::min(valid_bytes_, output_buffer_size);
        std::memcpy(output_buffer, span.data(), copy_size);
        buffer_pos_ = copy_size;

        DFTRACER_UTILS_LOG_DEBUG(
            "Got new chunk via zero-copy, copied %zu bytes (total in buffer: "
            "%zu)",
            copy_size, valid_bytes_);

        return copy_size;
    }

   private:
    std::size_t read_line_aligned_data() {
        if (!decompression_initialized_) {
            throw ReaderError(ReaderError::INITIALIZATION_ERROR,
                              "Streaming session not properly initialized");
        }

        if (is_at_target_end()) {
            DFTRACER_UTILS_LOG_DEBUG(
                "GzipLineByteStream: at target end, current_position=%zu, "
                "target_end_bytes=%zu",
                current_position_, target_end_bytes_);
            is_finished_ = true;
            return 0;
        }

        // Copy partial line buffer to internal buffer (if exists)
        std::size_t partial_size = partial_line_buffer_.size();
        std::size_t available_buffer_space = buffer_.size();

        if (partial_size > 0) {
            if (partial_size > buffer_.size()) {
                throw ReaderError(
                    ReaderError::READ_ERROR,
                    "Partial line buffer exceeds available buffer space");
            }
            std::memcpy(buffer_.data(), partial_line_buffer_.data(),
                        partial_size);
            available_buffer_space -= partial_size;
        }

        // Read data directly into internal buffer - stay strictly within target
        // bounds
        std::size_t max_bytes_to_read = target_end_bytes_ - current_position_;
        std::size_t bytes_to_read =
            std::min(max_bytes_to_read, available_buffer_space);

        std::size_t bytes_read = 0;
        if (bytes_to_read > 0) {
            bool status = inflater_.read(file_handle_,
                                         reinterpret_cast<unsigned char *>(
                                             buffer_.data() + partial_size),
                                         bytes_to_read, bytes_read);

            if (!status || bytes_read == 0) {
                is_finished_ = true;
                return 0;
            }
        }

        std::size_t total_data_size = partial_size + bytes_read;

        DFTRACER_UTILS_LOG_DEBUG(
            "Read %zu bytes from compressed stream, partial_buffer_size=%zu, "
            "current_position=%zu, target_end=%zu, total_data_size=%zu, "
            "bytes_returned=%zu",
            bytes_read, partial_size, current_position_, target_end_bytes_,
            total_data_size, bytes_returned_);

        current_position_ += bytes_read;

        // Apply boundary limits - only return complete lines within our chunk
        std::size_t adjusted_size =
            apply_range_and_boundary_limits(buffer_.data(), total_data_size);

        // Update partial buffer with any incomplete line data
        update_partial_buffer(buffer_.data(), adjusted_size, total_data_size);

        if (adjusted_size == 0) {
            DFTRACER_UTILS_LOG_DEBUG(
                "%s",
                "No complete line found in current buffer, will read more data "
                "on next call");
            return 0;
        }

        bytes_returned_ += adjusted_size;

        DFTRACER_UTILS_LOG_DEBUG(
            "Returning %zu bytes, total bytes_returned=%zu", adjusted_size,
            bytes_returned_);

        return adjusted_size;
    }

    // Zero-copy read - returns span to internal buffer
    dftracer::utils::span_view<const char> read() override {
        if (is_finished_) {
            return {};
        }

        // Read line-aligned data into buffer_
        valid_bytes_ = read_line_aligned_data();

        if (valid_bytes_ == 0) {
            return {};
        }

        // Return span view to the data in buffer_
        return span_view<const char>(buffer_.data(), valid_bytes_);
    }

    void reset() override {
        GzipStream::reset();
        partial_line_buffer_.clear();
        partial_line_buffer_.shrink_to_fit();
        valid_bytes_ = 0;
        buffer_pos_ = 0;
        actual_start_bytes_ = 0;
        bytes_returned_ = 0;
    }

    void update_partial_buffer(const char *buffer, std::size_t adjusted_size,
                               std::size_t total_data_size) {
        if (adjusted_size < total_data_size) {
            std::size_t remaining_size = total_data_size - adjusted_size;
            partial_line_buffer_.resize(remaining_size);
            std::memcpy(partial_line_buffer_.data(), buffer + adjusted_size,
                        remaining_size);
        } else {
            partial_line_buffer_.clear();
        }
    }

    std::size_t adjust_to_boundary(char *buffer, std::size_t buffer_size) {
        std::size_t newline_pos = SIZE_MAX;
        for (int64_t i = static_cast<int64_t>(buffer_size) - 1; i >= 0; i--) {
            if (buffer[i] == '\n') {
                newline_pos = static_cast<std::size_t>(i) + 1;
                break;
            }
        }

        if (newline_pos != SIZE_MAX) {
            bool at_file_end = target_end_bytes_ >= max_file_bytes_;
            std::size_t remaining = buffer_size - newline_pos;
            if (remaining > 0 && at_file_end) {
                return buffer_size;
            }
            return newline_pos;
        }

        bool at_file_end = target_end_bytes_ >= max_file_bytes_;
        if (is_finished_ || at_file_end) {
            return buffer_size;
        }

        return 0;
    }

    std::size_t apply_range_and_boundary_limits(char *buffer,
                                                std::size_t total_data_size) {
        // Strictly enforce that we only return complete lines that fall within
        // the file range [actual_start_bytes_, target_end_bytes_).
        //
        // IMPORTANT: current_position_ tracks where we've READ to in the file.
        // The buffer contains data ending at current_position_.
        // We must ensure all returned data ends before target_end_bytes_.

        // Check if we've already passed the boundary
        if (current_position_ > target_end_bytes_) {
            // We've read past the boundary, need to truncate
            // The buffer spans from (current_position_ - total_data_size) to
            // current_position_ We can only use the part before
            // target_end_bytes_

            // Note: partial_line_buffer_ is from a previous read position,
            // current read spans [current_position_ - (total_data_size -
            // partial_size), current_position_)

            std::size_t partial_size = partial_line_buffer_.size();
            std::size_t new_data_size = total_data_size - partial_size;
            std::size_t new_data_start = current_position_ - new_data_size;

            if (new_data_start >= target_end_bytes_) {
                // All new data is beyond the boundary
                is_finished_ = true;
                return 0;
            }

            // Only use data up to the boundary
            std::size_t usable_new_data = target_end_bytes_ - new_data_start;
            std::size_t limited_size =
                partial_size + usable_new_data;  // Include partial buffer

            // Find last complete line within the limit
            std::size_t adjusted_size =
                adjust_to_boundary(buffer, limited_size);

            // If no complete line found but we're at EOF, return what we have
            bool at_file_end = target_end_bytes_ >= max_file_bytes_;
            if (adjusted_size == 0 && limited_size > 0 &&
                (is_finished_ || at_file_end)) {
                return limited_size;
            }

            return adjusted_size;
        }

        // We haven't exceeded the boundary yet
        // Return all complete lines in the buffer
        std::size_t adjusted_size = adjust_to_boundary(buffer, total_data_size);

        return adjusted_size;
    }
};

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_GZIP_LINE_BYTE_STREAM_H
