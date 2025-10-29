#ifndef DFTRACER_UTILS_READER_STREAMS_GZIP_LINE_BYTE_STREAM_H
#define DFTRACER_UTILS_READER_STREAMS_GZIP_LINE_BYTE_STREAM_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/reader/streams/gzip_stream.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

class GzipLineByteStream : public GzipStream {
   private:
    static constexpr std::size_t SEARCH_BUFFER_SIZE = 2048;
    static constexpr std::size_t LINE_SEARCH_LOOKBACK = 512;

    std::vector<char> partial_line_buffer_;
    std::size_t actual_start_bytes_;

   public:
    GzipLineByteStream() : GzipStream(), actual_start_bytes_(0) {
        partial_line_buffer_.reserve(1 * 1024 * 1024);
    }

    void initialize(const std::string &gz_path, std::size_t start_bytes,
                    std::size_t end_bytes,
                    dftracer::utils::Indexer &indexer) override {
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

    std::size_t read(char *buffer, std::size_t buffer_size) override {
// Prefetch buffer for writing
#ifdef __GNUC__
        __builtin_prefetch(buffer, 1, 3);
#endif

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

        // Copy partial line buffer directly to output buffer (if exists)
        std::size_t partial_size = partial_line_buffer_.size();
        std::size_t available_buffer_space = buffer_size;

        if (partial_size > 0) {
            if (partial_size > buffer_size) {
                throw ReaderError(
                    ReaderError::READ_ERROR,
                    "Partial line buffer exceeds available buffer space");
            }
            std::memcpy(buffer, partial_line_buffer_.data(), partial_size);
            available_buffer_space -= partial_size;
        }

        // Read data directly into output buffer - initially try to stay within
        // target bounds
        std::size_t max_bytes_to_read = target_end_bytes_ - current_position_;
        std::size_t bytes_to_read =
            std::min(max_bytes_to_read, available_buffer_space);

        std::size_t bytes_read = 0;
        if (bytes_to_read > 0) {
            bool status = inflater_.read(
                file_handle_,
                reinterpret_cast<unsigned char *>(buffer + partial_size),
                bytes_to_read, bytes_read);

            if (!status || bytes_read == 0) {
                is_finished_ = true;
                return 0;
            }
        }

        std::size_t total_data_size = partial_size + bytes_read;

        DFTRACER_UTILS_LOG_DEBUG(
            "Read %zu bytes from compressed stream, partial_buffer_size=%zu, "
            "current_position=%zu, target_end=%zu, total_data_size=%zu",
            bytes_read, partial_size, current_position_, target_end_bytes_,
            total_data_size);

        std::size_t adjusted_size =
            apply_range_and_boundary_limits(buffer, total_data_size);

        current_position_ += bytes_read;

        // Update partial buffer with any incomplete line data
        update_partial_buffer(buffer, adjusted_size, total_data_size);

        if (adjusted_size == 0) {
            DFTRACER_UTILS_LOG_DEBUG(
                "%s",
                "No complete line found in current buffer, will read more data "
                "on next call");
            return 0;
        }

        return adjusted_size;
    }

    void reset() override {
        GzipStream::reset();
        partial_line_buffer_.clear();
        partial_line_buffer_.shrink_to_fit();
        actual_start_bytes_ = 0;
    }

   private:
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
        std::size_t adjusted_size;

        if (current_position_ < actual_start_bytes_) {
            DFTRACER_UTILS_LOG_ERROR(
                "Invalid state: current_position_ %zu < "
                "actual_start_bytes_ %zu",
                current_position_, actual_start_bytes_);
            throw ReaderError(ReaderError::READ_ERROR,
                              "Invalid internal position state detected");
        }

        // Calculate how much data we've already returned from this chunk
        std::size_t bytes_already_returned =
            current_position_ - actual_start_bytes_;

        // The maximum data this chunk should return is limited by the original
        // chunk boundary We stop at target_end_bytes_ to prevent overlaps with
        // the next chunk
        std::size_t max_position_allowed = target_end_bytes_;
        std::size_t max_allowed_return = 0;

        if (actual_start_bytes_ < max_position_allowed) {
            std::size_t total_range_for_chunk =
                max_position_allowed - actual_start_bytes_;
            max_allowed_return =
                (bytes_already_returned < total_range_for_chunk)
                    ? (total_range_for_chunk - bytes_already_returned)
                    : 0;
        }

        std::size_t limited_data_size =
            std::min(total_data_size, max_allowed_return);
        adjusted_size = adjust_to_boundary(buffer, limited_data_size);
        return adjusted_size;
    }
};

#endif  // DFTRACER_UTILS_READER_STREAMS_GZIP_LINE_BYTE_STREAM_H
