// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_GZIP_LINE_BYTE_STREAM_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_GZIP_LINE_BYTE_STREAM_H

#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/utilities/reader/internal/streams/gzip_stream.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dftracer::utils::utilities::reader::internal {

class GzipLineByteStream : public GzipStream {
   private:
    static constexpr std::size_t SEARCH_BUFFER_SIZE = 2048;

    dstd::Vector<char> partial_line_buffer_;
    std::size_t actual_start_bytes_;
    std::size_t bytes_returned_;  // Track how many bytes we've returned to user
    bool extend_to_line_boundary_ = false;
    bool open_at_earlier_piece_ = false;

   public:
    GzipLineByteStream(std::size_t buffer_size = DEFAULT_BUFFER_SIZE)
        : GzipStream(buffer_size > 0 ? buffer_size : DEFAULT_BUFFER_SIZE),
          actual_start_bytes_(0),
          bytes_returned_(0) {
        partial_line_buffer_.reserve(1 * 1024 * 1024);
    }

    using GzipStream::read_async;

    void set_extend_to_line_boundary(bool v) { extend_to_line_boundary_ = v; }

    /// Start at the line holding the target even when that line opens in an
    /// earlier piece, so adjacent ranges that each leave their last partial
    /// line to the next one return every line once.
    void set_open_at_earlier_piece(bool v) { open_at_earlier_piece_ = v; }

    void initialize(
        const dstd::String &gz_path, std::size_t start_bytes,
        std::size_t end_bytes,
        dftracer::utils::index::gzip::CheckpointIndexer &indexer) override {
        GzipStream::initialize(gz_path, start_bytes, end_bytes, indexer);
        actual_start_bytes_ = start_bytes;
        current_position_ = start_bytes;
    }

    coro::CoroTask<void> on_initialized() override {
        if (is_finished_) co_return;
        actual_start_bytes_ = co_await find_line_start(start_bytes_);
        current_position_ = actual_start_bytes_;
    }

    coro::CoroTask<std::size_t> find_line_start(std::size_t target_start) {
        // Unless asked for the line under the target, a piece that starts
        // inside a line owns its lines from the first newline on: the caller
        // counts lines from there, or the piece before finishes the line.
        if (mid_line_start_ && !open_at_earlier_piece_) {
            const std::size_t first = co_await first_line_start_after_anchor();
            co_await restart_compression();
            if (target_start < first) {
                if (first > seek_anchor_offset()) co_await skip(first);
                co_return first;
            }
        }

        if (!mid_line_start_ && target_start <= seek_anchor_offset()) {
            co_return target_start;
        }

        // A line has no size limit, so only the data from the anchor can show
        // where the line holding the target opens. A skip decodes it anyway.
        // A range ending inside a line leaves that line to the next range, so
        // a line that opens before this piece is read from an earlier one.
        std::size_t actual_start = seek_anchor_offset();
        while (true) {
            std::size_t current_pos = seek_anchor_offset();
            bool found = false;
            while (current_pos < target_start) {
                std::size_t got = 0;
                const bool read_ok = co_await inflater_.read(
                    fd_.native_handle(), file_offset_,
                    reinterpret_cast<unsigned char *>(buffer_.data()),
                    std::min(buffer_.size(), target_start - current_pos), got);
                if (!read_ok) {
                    throw ReaderError(
                        ReaderError::CompressionError,
                        "gzip decode failed mid-stream at position " +
                            dstd::to_string(current_pos));
                }
                if (got == 0) break;
                for (std::size_t i = got; i > 0; --i) {
                    if (buffer_[i - 1] == '\n') {
                        actual_start = current_pos + i;
                        found = true;
                        break;
                    }
                }
                current_pos += got;
            }
            if (found || !mid_line_start_) break;
            if (!indexer_->find_member(seek_anchor_offset() - 1, member_)) {
                throw ReaderError(ReaderError::CompressionError,
                                  "Failed to find the piece before position " +
                                      dstd::to_string(seek_anchor_offset()));
            }
            co_await restart_compression();
            mid_line_start_ = piece_starts_mid_line();
            actual_start = seek_anchor_offset();
        }

        co_await restart_compression();
        if (actual_start > seek_anchor_offset()) {
            co_await skip(actual_start);
        }

        co_return actual_start;
    }

   private:
    /// Uncompressed offset just past the first newline of the piece the
    /// stream sits at, read from its start. The end of the file when there is
    /// none. Leaves the stream past that data, so the caller restarts it.
    coro::CoroTask<std::size_t> first_line_start_after_anchor() {
        std::size_t pos = seek_anchor_offset();
        alignas(DFTRACER_UTILS_OPTIMAL_ALIGNMENT) unsigned char
            chunk[SEARCH_BUFFER_SIZE];
        while (pos < max_file_bytes_) {
            std::size_t got = 0;
            const bool ok = co_await inflater_.read(
                fd_.native_handle(), file_offset_, chunk, sizeof(chunk), got);
            if (!ok || got == 0) break;
            const void *nl = std::memchr(chunk, '\n', got);
            if (nl) {
                co_return pos +
                    static_cast<std::size_t>(
                        static_cast<const unsigned char *>(nl) - chunk) +
                    1;
            }
            pos += got;
        }
        co_return max_file_bytes_;
    }

    coro::CoroTask<std::size_t> read_line_aligned_data() {
        if (!decompression_initialized_) {
            throw ReaderError(ReaderError::InitializationError,
                              "Streaming session not properly initialized");
        }

        if (is_at_target_end()) {
            if (extend_to_line_boundary_ && !partial_line_buffer_.empty() &&
                current_position_ < max_file_bytes_) {
                std::size_t partial_size = partial_line_buffer_.size();
                grow_buffer_past(partial_size);
                std::memcpy(buffer_.data(), partial_line_buffer_.data(),
                            partial_size);
                std::size_t total = partial_size;
                std::size_t emit = 0;
                while (emit == 0 && current_position_ < max_file_bytes_) {
                    if (total == buffer_.size()) grow_buffer_past(total);
                    std::size_t cap = static_cast<std::size_t>(
                        max_file_bytes_ - current_position_);
                    std::size_t to_read = std::min(buffer_.size() - total, cap);
                    std::size_t got = 0;
                    bool ok = co_await inflater_.read(
                        fd_.native_handle(), file_offset_,
                        reinterpret_cast<unsigned char *>(buffer_.data() +
                                                          total),
                        to_read, got);
                    if (!ok || got == 0) break;
                    current_position_ += got;
                    for (std::size_t i = total; i < total + got; ++i) {
                        if (buffer_[i] == '\n') {
                            emit = i + 1;
                            break;
                        }
                    }
                    total += got;
                }
                partial_line_buffer_.clear();
                is_finished_ = true;
                if (emit > 0) {
                    bytes_returned_ += emit;
                    co_return emit;
                }
                co_return 0;
            }
            is_finished_ = true;
            co_return 0;
        }

        // Copy partial line buffer to internal buffer (if exists)
        std::size_t partial_size = partial_line_buffer_.size();

        if (partial_size > 0) {
            grow_buffer_past(partial_size);
            std::memcpy(buffer_.data(), partial_line_buffer_.data(),
                        partial_size);
        }
        std::size_t available_buffer_space = buffer_.size() - partial_size;

        // Read data directly into internal buffer - stay strictly within target
        // bounds
        std::size_t max_bytes_to_read = target_end_bytes_ - current_position_;
        std::size_t bytes_to_read =
            std::min(max_bytes_to_read, available_buffer_space);

        std::size_t bytes_read = 0;
        if (bytes_to_read > 0) {
            bool status =
                co_await inflater_.read(fd_.native_handle(), file_offset_,
                                        reinterpret_cast<unsigned char *>(
                                            buffer_.data() + partial_size),
                                        bytes_to_read, bytes_read);

            // A decode failure must not pass for clean EOF (which would
            // silently drop the rest of the file).
            if (!status) {
                throw ReaderError(ReaderError::CompressionError,
                                  "gzip decode failed mid-stream at position " +
                                      dstd::to_string(current_position_));
            }
            if (bytes_read == 0) {
                is_finished_ = true;
                co_return 0;
            }
        }

        std::size_t total_data_size = partial_size + bytes_read;

        DFTRACER_UTILS_LOG_DEBUG(
            "Read {} bytes from compressed stream, partial_buffer_size={}, "
            "current_position={}, target_end={}, total_data_size={}, "
            "bytes_returned={}",
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
                "{}",
                "No complete line found in current buffer, will read more data "
                "on next call");
            co_return 0;
        }

        bytes_returned_ += adjusted_size;

        DFTRACER_UTILS_LOG_DEBUG("Returning {} bytes, total bytes_returned={}",
                                 adjusted_size, bytes_returned_);

        co_return adjusted_size;
    }

    coro::CoroTask<::std::span<const char>> read_async() override {
        co_await ensure_initialized();
        if (is_finished_) {
            co_return {};
        }

        // A read that ends inside a line returns nothing yet; an empty span
        // ends the stream, so read on until a line completes or the range
        // ends.
        do {
            valid_bytes_ = co_await read_line_aligned_data();
        } while (valid_bytes_ == 0 && !is_finished_);

        if (valid_bytes_ == 0) {
            co_return {};
        }

        // Return view to the data in buffer_
        co_return dstd::Span<const char>(buffer_.data(), valid_bytes_);
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

    /// A line has no size limit: a partial line that fills the buffer doubles
    /// it, and the buffer keeps that size for the lines after.
    void grow_buffer_past(std::size_t partial_size) {
        if (partial_size < buffer_.size()) return;
        std::size_t size = buffer_.size();
        while (size <= partial_size) size *= 2;
        buffer_.resize(size);
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
            bool at_file_end = current_position_ >= max_file_bytes_;
            std::size_t remaining = buffer_size - newline_pos;
            if (remaining > 0 && at_file_end) {
                return buffer_size;
            }
            return newline_pos;
        }

        bool at_file_end = current_position_ >= max_file_bytes_;
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
