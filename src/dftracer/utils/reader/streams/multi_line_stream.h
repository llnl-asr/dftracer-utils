#ifndef DFTRACER_UTILS_READER_STREAMS_MULTI_LINE_STREAM_H
#define DFTRACER_UTILS_READER_STREAMS_MULTI_LINE_STREAM_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/reader/stream.h>

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
    std::vector<char> read_buffer_;
    std::string line_accumulator_;
    bool is_finished_;
    std::size_t current_line_;
    std::size_t start_line_;
    std::size_t end_line_;
    std::size_t initial_line_;
    std::size_t lines_output_;
    std::size_t buffer_pos_;
    std::size_t buffer_size_;
    static constexpr std::size_t DEFAULT_READ_BUFFER_SIZE = 1 * 1024 * 1024;

   public:
    explicit MultiLineStream(std::unique_ptr<ReaderStream> underlying_stream,
                             std::size_t start_line = 0,
                             std::size_t end_line = 0,
                             std::size_t initial_line = 1)
        : underlying_stream_(std::move(underlying_stream)),
          read_buffer_(DEFAULT_READ_BUFFER_SIZE),
          is_finished_(false),
          current_line_(initial_line),
          start_line_(start_line),
          end_line_(end_line),
          initial_line_(initial_line),
          lines_output_(0),
          buffer_pos_(0),
          buffer_size_(0) {
        line_accumulator_.reserve(1024);
    }

    ~MultiLineStream() override { reset(); }

    std::size_t read(char* buffer, std::size_t buffer_size) override {
        if (!underlying_stream_ || is_finished_) {
            return 0;
        }

        const std::size_t max_lines = calculate_max_lines();
        std::size_t bytes_written = 0;

        // Main processing loop: read and output lines until done or buffer full
        while (should_continue_reading(max_lines) &&
               bytes_written < buffer_size) {
            if (!refill_buffer_if_needed()) {
                break;
            }

            if (!process_buffer_lines(buffer, buffer_size, bytes_written,
                                      max_lines)) {
                break;
            }
        }

        // Handle any final line at EOF
        handle_final_line(buffer, buffer_size, bytes_written, max_lines);

        // Update finished state
        update_finish_state(max_lines);

        return bytes_written;
    }

    bool done() const override {
        return is_finished_ ||
               (underlying_stream_ && underlying_stream_->done() &&
                line_accumulator_.empty());
    }

    void reset() override {
        if (underlying_stream_) {
            underlying_stream_->reset();
        }
        line_accumulator_.clear();
        is_finished_ = false;
        current_line_ = initial_line_;
        lines_output_ = 0;
        buffer_pos_ = 0;
        buffer_size_ = 0;
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

    // ========================================================================
    // Buffer Management
    // ========================================================================

    bool refill_buffer_if_needed() {
        if (buffer_pos_ < buffer_size_) {
            return true;
        }

        buffer_size_ =
            underlying_stream_->read(read_buffer_.data(), read_buffer_.size());
        buffer_pos_ = 0;
        return buffer_size_ > 0;
    }

    const char* find_next_newline() const {
        return static_cast<const char*>(
            std::memchr(read_buffer_.data() + buffer_pos_, '\n',
                        buffer_size_ - buffer_pos_));
    }

    void accumulate_remaining_buffer() {
        line_accumulator_.append(read_buffer_.data() + buffer_pos_,
                                 buffer_size_ - buffer_pos_);
        buffer_pos_ = buffer_size_;
    }

    // ========================================================================
    // Line Processing
    // ========================================================================

    /**
     * @brief Process all complete lines in the current buffer.
     *
     * Scans for newlines, outputs lines in range, and skips filtered lines.
     *
     * @return false if finished or buffer full, true to continue reading
     */
    bool process_buffer_lines(char* buffer, std::size_t buffer_size,
                              std::size_t& bytes_written,
                              std::size_t max_lines) {
        while (buffer_pos_ < buffer_size_) {
            const char* newline_ptr = find_next_newline();

            if (!newline_ptr) {
                // No complete line, accumulate and read more data
                accumulate_remaining_buffer();
                break;
            }

            std::size_t newline_pos = newline_ptr - read_buffer_.data();
            std::size_t line_len = newline_pos - buffer_pos_;

            // Determine if this line should be output
            bool in_range = is_line_in_output_range();
            bool can_output = lines_output_ < max_lines;

            if (in_range && can_output) {
                if (!try_output_line(buffer, buffer_size, bytes_written,
                                     line_len)) {
                    // Buffer full, return and continue next call
                    return false;
                }
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
            buffer_pos_ = newline_pos + 1;

            // Check if we've output enough lines
            if (reached_max_lines(max_lines)) {
                is_finished_ = true;
                return false;
            }
        }

        return true;
    }

    /**
     * @brief Attempt to output a line to the buffer.
     *
     * Fast path: Direct copy from read_buffer_ when no accumulated data.
     * Slow path: Complete accumulated line first, then copy.
     *
     * @return true if line was output, false if buffer is full
     */
    bool try_output_line(char* buffer, std::size_t buffer_size,
                         std::size_t& bytes_written, std::size_t line_len) {
        // Fast path: direct copy when no accumulated data
        if (line_accumulator_.empty()) {
            if (bytes_written + line_len + 1 > buffer_size) {
                // Buffer full - save for next call
                line_accumulator_.append(read_buffer_.data() + buffer_pos_,
                                         line_len);
                return false;
            }

            // Direct copy: read_buffer_ → output buffer
            std::memcpy(buffer + bytes_written,
                        read_buffer_.data() + buffer_pos_, line_len);
            bytes_written += line_len;
            buffer[bytes_written++] = '\n';
            lines_output_++;
            return true;
        }

        // Slow path: complete and output accumulated line
        line_accumulator_.append(read_buffer_.data() + buffer_pos_, line_len);
        std::size_t total_len = line_accumulator_.size();

        if (bytes_written + total_len + 1 > buffer_size) {
            // Buffer full - keep accumulated for next call
            return false;
        }

        // Output complete accumulated line
        std::memcpy(buffer + bytes_written, line_accumulator_.c_str(),
                    total_len);
        bytes_written += total_len;
        buffer[bytes_written++] = '\n';
        lines_output_++;
        line_accumulator_.clear();
        return true;
    }

    // ========================================================================
    // EOF and State Management
    // ========================================================================

    void handle_final_line(char* buffer, std::size_t buffer_size,
                           std::size_t& bytes_written, std::size_t max_lines) {
        // Only handle final line if all conditions are met
        if (!underlying_stream_->done() || line_accumulator_.empty() ||
            !is_line_in_output_range() || reached_max_lines(max_lines)) {
            return;
        }

        std::size_t len = line_accumulator_.size();
        if (bytes_written + len <= buffer_size) {
            std::memcpy(buffer + bytes_written, line_accumulator_.c_str(), len);
            bytes_written += len;
            line_accumulator_.clear();
            lines_output_++;
        }
    }

    void update_finish_state(std::size_t max_lines) {
        if (underlying_stream_->done() || reached_end_of_range() ||
            reached_max_lines(max_lines)) {
            is_finished_ = true;
        }
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_READER_STREAMS_MULTI_LINE_STREAM_H
