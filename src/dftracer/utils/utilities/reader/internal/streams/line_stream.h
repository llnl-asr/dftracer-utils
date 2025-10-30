#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_LINE_STREAM_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_LINE_STREAM_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/span.h>
#include <dftracer/utils/utilities/reader/internal/stream.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::reader::internal {

/**
 * @brief Stream that returns one single line at a time from a LINE_BYTES
 * stream.
 *
 * Wraps a LINE_BYTES stream and provides single-line reading.
 * Each call to read() returns exactly one complete line (with newline).
 * Can optionally filter by line range when line numbers are specified.
 */
class LineStream : public ReaderStream {
   private:
    std::unique_ptr<internal::ReaderStream> underlying_stream_;
    span_view<const char> current_span_;  // Current span from underlying stream
    std::string line_accumulator_;
    std::string current_line_;
    bool is_finished_;
    bool has_pending_line_;
    std::size_t current_line_number_;
    std::size_t start_line_;
    std::size_t end_line_;
    std::size_t initial_line_;
    std::size_t output_position_;
    std::size_t span_pos_;  // Position within current span

   public:
    explicit LineStream(std::unique_ptr<ReaderStream> underlying_stream,
                        std::size_t start_line = 0, std::size_t end_line = 0,
                        std::size_t initial_line = 1)
        : underlying_stream_(std::move(underlying_stream)),
          current_span_(),
          is_finished_(false),
          has_pending_line_(false),
          current_line_number_(initial_line),
          start_line_(start_line),
          end_line_(end_line),
          initial_line_(initial_line),
          output_position_(0),
          span_pos_(0) {
        line_accumulator_.reserve(1024);
        current_line_.reserve(1024);
    }

    ~LineStream() override { reset(); }

    // Zero-copy read - returns view to current_line_
    span_view<const char> read() override {
        if (!underlying_stream_) {
            return {};
        }

        if (is_finished_) {
            return {};
        }

        // Parse next line into current_line_
        if (!parse_next_line()) {
            return {};
        }

        // Return span view to current_line_
        return span_view<const char>(current_line_.data(),
                                     current_line_.size());
    }

    std::size_t read(char* buffer, std::size_t buffer_size) override {
        if (!underlying_stream_) {
            return 0;
        }

        // Handle any pending line from previous call
        if (has_pending_line_) {
            return output_pending_line(buffer, buffer_size);
        }

        if (is_finished_) {
            return 0;
        }

        // Try fast path: direct output from read_buffer_ to output buffer
        std::size_t written = try_direct_output(buffer, buffer_size);

        if (written > 0) {
            return written;
        }

        // Slow path: need to use intermediate storage
        if (!parse_next_line()) {
            return 0;
        }

        return output_pending_line(buffer, buffer_size);
    }

    bool done() const override { return is_finished_ && !has_pending_line_; }

    void reset() override {
        if (underlying_stream_) {
            underlying_stream_->reset();
        }
        line_accumulator_.clear();
        current_line_.clear();
        current_span_ = span_view<const char>();
        is_finished_ = false;
        has_pending_line_ = false;
        current_line_number_ = initial_line_;
        output_position_ = 0;
        span_pos_ = 0;
    }

   private:
    // ========================================================================
    // Range Checking Helpers
    // ========================================================================

    bool is_beyond_range() const {
        return end_line_ > 0 && current_line_number_ > end_line_;
    }

    bool should_output_current_line() const {
        if (start_line_ == 0 && end_line_ == 0) {
            return true;
        }

        bool after_start =
            (start_line_ == 0 || current_line_number_ >= start_line_);
        bool before_end = (end_line_ == 0 || current_line_number_ <= end_line_);

        return after_start && before_end;
    }

    // ========================================================================
    // Buffer Management
    // ========================================================================

    bool refill_span_if_needed() {
        if (span_pos_ < current_span_.size()) {
            return true;
        }

        if (underlying_stream_->done()) {
            return false;
        }

        // Get new span from underlying stream (zero-copy)
        current_span_ = underlying_stream_->read();
        span_pos_ = 0;
        return !current_span_.empty();
    }

    const char* find_next_newline() const {
        return static_cast<const char*>(
            std::memchr(current_span_.data() + span_pos_, '\n',
                        current_span_.size() - span_pos_));
    }

    void accumulate_remaining_span() {
        line_accumulator_.append(current_span_.data() + span_pos_,
                                 current_span_.size() - span_pos_);
        span_pos_ = current_span_.size();
    }

    // ========================================================================
    // Fast Path: Direct Output (No Intermediate Storage)
    // ========================================================================

    /**
     * @brief Attempt to write a line directly from read_buffer_ to output
     * buffer.
     *
     * This fast path avoids intermediate string copies when:
     * - No accumulated data exists
     * - A complete line fits in the output buffer
     *
     * Uses a loop to skip filtered lines efficiently.
     *
     * @return Number of bytes written, or 0 if fast path unavailable
     */
    std::size_t try_direct_output(char* buffer, std::size_t buffer_size) {
        // Fast path requires no accumulated data
        if (!line_accumulator_.empty()) {
            return 0;
        }

        // Loop to skip filtered lines efficiently
        while (true) {
            if (is_beyond_range()) {
                is_finished_ = true;
                return 0;
            }

            if (!refill_span_if_needed()) {
                return 0;
            }

            const char* newline_ptr = find_next_newline();
            if (!newline_ptr) {
                // No complete line available, must use slow path
                return 0;
            }

            std::size_t newline_pos = newline_ptr - current_span_.data();
            std::size_t line_length = newline_pos - span_pos_ + 1;

            // Line must fit in output buffer for fast path
            if (line_length > buffer_size) {
                return 0;
            }

            bool should_output = should_output_current_line();

            if (is_beyond_range()) {
                is_finished_ = true;
                return 0;
            }

            current_line_number_++;

            if (should_output) {
                // Direct copy: span → output buffer (zero-copy from underlying
                // stream!)
                std::memcpy(buffer, current_span_.data() + span_pos_,
                            line_length);
                span_pos_ = newline_pos + 1;
                return line_length;
            }

            // Line filtered out, skip and continue to next
            span_pos_ = newline_pos + 1;
        }
    }

    // ========================================================================
    // Slow Path: Parse and Store Line
    // ========================================================================

    void finalize_line_with_accumulator(std::size_t line_length) {
        line_accumulator_.append(current_span_.data() + span_pos_, line_length);
        line_accumulator_.push_back('\n');
        current_line_ = std::move(line_accumulator_);
        line_accumulator_.clear();
    }

    void finalize_line_direct(std::size_t line_length) {
        current_line_.assign(current_span_.data() + span_pos_, line_length);
        current_line_.push_back('\n');
    }

    bool process_complete_line(std::size_t newline_pos) {
        std::size_t line_length = newline_pos - span_pos_;

        // Build complete line with or without accumulated data
        if (!line_accumulator_.empty()) {
            finalize_line_with_accumulator(line_length);
        } else {
            finalize_line_direct(line_length);
        }

        span_pos_ = newline_pos + 1;

        bool should_output = should_output_current_line();

        if (is_beyond_range()) {
            is_finished_ = true;
            return false;
        }

        current_line_number_++;

        if (should_output) {
            has_pending_line_ = true;
            output_position_ = 0;
            return true;
        }

        // Line filtered out, continue parsing
        current_line_.clear();
        return false;
    }

    bool parse_next_line() {
        if (is_beyond_range()) {
            is_finished_ = true;
            return false;
        }

        while (true) {
            if (!refill_span_if_needed()) {
                break;
            }

            // Process all complete lines in current span
            while (span_pos_ < current_span_.size()) {
                const char* newline_ptr = find_next_newline();

                if (!newline_ptr) {
                    accumulate_remaining_span();
                    break;
                }

                std::size_t newline_pos = newline_ptr - current_span_.data();

                if (process_complete_line(newline_pos)) {
                    return true;
                }
            }
        }

        // Handle final line at EOF without trailing newline
        if (underlying_stream_->done() && !line_accumulator_.empty()) {
            current_line_ = std::move(line_accumulator_);
            line_accumulator_.clear();

            if (should_output_current_line() && !is_beyond_range()) {
                has_pending_line_ = true;
                output_position_ = 0;
                current_line_number_++;
                return true;
            }
        }

        is_finished_ = true;
        return false;
    }

    // ========================================================================
    // Output Helpers
    // ========================================================================

    std::size_t output_pending_line(char* buffer, std::size_t buffer_size) {
        if (output_position_ >= current_line_.size()) {
            has_pending_line_ = false;
            current_line_.clear();
            output_position_ = 0;
            return 0;
        }

        std::size_t remaining = current_line_.size() - output_position_;
        std::size_t copy_size = std::min(remaining, buffer_size);

        std::memcpy(buffer, current_line_.data() + output_position_, copy_size);
        output_position_ += copy_size;

        if (output_position_ >= current_line_.size()) {
            has_pending_line_ = false;
            current_line_.clear();
            output_position_ = 0;
        }

        return copy_size;
    }
};

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_LINE_STREAM_H
