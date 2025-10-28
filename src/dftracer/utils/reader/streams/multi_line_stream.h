#ifndef DFTRACER_UTILS_READER_STREAMS_MULTI_LINE_STREAM_H
#define DFTRACER_UTILS_READER_STREAMS_MULTI_LINE_STREAM_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/span.h>
#include <dftracer/utils/reader/stream.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace dftracer::utils {

/**
 * @brief Stream that parses multiple lines from a LINE_BYTES stream.
 *
 * Wraps a LINE_BYTES stream and provides multi-line reading.
 * Each call to read() returns multiple complete lines (with newlines).
 * Can optionally filter by line range when line numbers are specified.
 */
class MultiLineStream : public ReaderStream {
   private:
    std::unique_ptr<ReaderStream> underlying_stream_;
    span_view<const char> current_span_;  // Current span from underlying stream
    std::string line_accumulator_;
    std::string output_buffer_;
    bool is_finished_;
    std::size_t current_line_;
    std::size_t start_line_;
    std::size_t end_line_;
    std::size_t initial_line_;
    std::size_t lines_output_;
    std::size_t span_pos_;
    std::size_t
        output_buf_pos_;  // Position in output_buffer_ for buffer-based reads

   public:
    explicit MultiLineStream(std::unique_ptr<ReaderStream> underlying_stream,
                             std::size_t start_line = 0,
                             std::size_t end_line = 0,
                             std::size_t initial_line = 1)
        : underlying_stream_(std::move(underlying_stream)),
          current_span_(),
          is_finished_(false),
          current_line_(initial_line),
          start_line_(start_line),
          end_line_(end_line),
          initial_line_(initial_line),
          lines_output_(0),
          span_pos_(0),
          output_buf_pos_(0) {
        line_accumulator_.reserve(1024);
        output_buffer_.reserve(10 * 1024 *
                               1024);  // 10MB to handle large line ranges
    }

    ~MultiLineStream() override { reset(); }

    span_view<const char> read() override {
        if (!underlying_stream_) {
            return {};
        }

        if (is_finished_) {
            return {};
        }

        output_buffer_.clear();
        const std::size_t max_lines = calculate_max_lines();

        // Read and accumulate multiple lines into output_buffer_
        while (!reached_end_of_range() && !reached_max_lines(max_lines)) {
            if (!refill_if_needed()) {
                break;
            }

            if (!process_lines(max_lines)) {
                break;
            }
        }

        // Handle final line at EOF
        handle_final_line(max_lines);

        // Update finish state
        update_finish_state();

        if (output_buffer_.empty()) {
            return {};
        }

        return span_view<const char>(output_buffer_.data(),
                                     output_buffer_.size());
    }

    std::size_t read(char* buffer, std::size_t buffer_size) override {
        if (!underlying_stream_) {
            return 0;
        }

        // Check if we have unconsumed data from previous read
        if (output_buf_pos_ < output_buffer_.size()) {
            std::size_t remaining = output_buffer_.size() - output_buf_pos_;
            std::size_t copy_size = std::min(remaining, buffer_size);
            std::memcpy(buffer, output_buffer_.data() + output_buf_pos_,
                        copy_size);
            output_buf_pos_ += copy_size;
            return copy_size;
        }

        // Buffer exhausted
        if (is_finished_) {
            return 0;
        }

        // Get new chunk via zero-copy read
        auto span = read();
        if (span.empty()) {
            return 0;
        }

        // Reset position for new buffer
        output_buf_pos_ = 0;

        // Copy from span to user buffer
        std::size_t copy_size = std::min(span.size(), buffer_size);
        std::memcpy(buffer, span.data(), copy_size);
        output_buf_pos_ = copy_size;

        return copy_size;
    }

    bool done() const override {
        // Check if we have buffered data that hasn't been consumed yet
        if (output_buf_pos_ < output_buffer_.size()) {
            return false;  // Still have data to return
        }

        // We're done when: underlying stream is done and no accumulated data
        return is_finished_ ||
               (underlying_stream_ && underlying_stream_->done() &&
                line_accumulator_.empty());
    }

    void reset() override {
        if (underlying_stream_) {
            underlying_stream_->reset();
        }
        line_accumulator_.clear();
        output_buffer_.clear();
        current_span_ = span_view<const char>();
        is_finished_ = false;
        current_line_ = initial_line_;
        lines_output_ = 0;
        span_pos_ = 0;
        output_buf_pos_ = 0;
    }

   private:
    // ========================================================================
    // Range and Limit Checking
    // ========================================================================

    std::size_t calculate_max_lines() const {
        return (end_line_ > 0 && start_line_ > 0)
                   ? (end_line_ - start_line_ + 1)
                   : SIZE_MAX;
    }

    bool should_continue_reading(std::size_t max_lines) const {
        return !underlying_stream_->done() && !reached_end_of_range() &&
               !reached_max_lines(max_lines);
    }

    bool reached_end_of_range() const {
        return end_line_ > 0 && current_line_ > end_line_;
    }

    bool reached_max_lines(std::size_t max_lines) const {
        return lines_output_ >= max_lines;
    }

    bool is_line_in_output_range() const {
        return current_line_ >= start_line_ &&
               (end_line_ == 0 || current_line_ <= end_line_);
    }

    bool refill_if_needed() {
        if (span_pos_ < current_span_.size()) {
            return true;
        }

        if (underlying_stream_->done()) {
            return false;
        }

        current_span_ = underlying_stream_->read();
        span_pos_ = 0;
        return !current_span_.empty();
    }

    const char* find_next_newline() const {
        return static_cast<const char*>(
            std::memchr(current_span_.data() + span_pos_, '\n',
                        current_span_.size() - span_pos_));
    }

    void accumulate_remaining() {
        line_accumulator_.append(current_span_.data() + span_pos_,
                                 current_span_.size() - span_pos_);
        span_pos_ = current_span_.size();
    }

    // ========================================================================
    // Line Processing
    // ========================================================================

    /**
     * @brief Process lines from current span and append to output_buffer_.
     *
     * @return false if finished or reached limits, true to continue reading
     */
    bool process_lines(std::size_t max_lines) {
        while (span_pos_ < current_span_.size()) {
            const char* newline_ptr = find_next_newline();

            if (!newline_ptr) {
                // No complete line, accumulate and read more data
                accumulate_remaining();
                break;
            }

            std::size_t newline_pos = newline_ptr - current_span_.data();
            std::size_t line_len = newline_pos - span_pos_;

            // Determine if this line should be output
            bool in_range = is_line_in_output_range();
            bool can_output = lines_output_ < max_lines;

            if (in_range && can_output) {
                // Output line to output_buffer_
                if (!line_accumulator_.empty()) {
                    // Complete accumulated line
                    line_accumulator_.append(current_span_.data() + span_pos_,
                                             line_len);
                    output_buffer_.append(line_accumulator_);
                    output_buffer_.push_back('\n');
                    line_accumulator_.clear();
                } else {
                    // Direct append from span
                    output_buffer_.append(current_span_.data() + span_pos_,
                                          line_len);
                    output_buffer_.push_back('\n');
                }
                lines_output_++;
            } else if (reached_end_of_range()) {
                // Past end of range, stop processing
                is_finished_ = true;
                return false;
            } else {
                // Line filtered out, skip it
                line_accumulator_.clear();
            }

            // Advance to next line
            current_line_++;
            span_pos_ = newline_pos + 1;

            // Check if we've output enough lines total
            if (reached_max_lines(max_lines)) {
                // Stop accumulating more lines
                return true;
            }
        }

        return true;
    }

    void handle_final_line(std::size_t max_lines) {
        if (!underlying_stream_->done() || line_accumulator_.empty() ||
            !is_line_in_output_range() || reached_max_lines(max_lines)) {
            return;
        }

        output_buffer_.append(line_accumulator_);
        line_accumulator_.clear();
        lines_output_++;
    }

    void update_finish_state() {
        // Only finish if underlying stream is done, not just because we hit
        // max_lines The max_lines limit is enforced by not outputting more
        // lines, but we can still have buffered data to return to the caller
        if (underlying_stream_->done() || reached_end_of_range()) {
            is_finished_ = true;
        }
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_READER_STREAMS_MULTI_LINE_STREAM_H
