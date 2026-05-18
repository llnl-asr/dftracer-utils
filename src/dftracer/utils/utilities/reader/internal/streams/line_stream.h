#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_LINE_STREAM_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_STREAMS_LINE_STREAM_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/reader/internal/stream.h>

#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::reader::internal {

class LineStream : public ReaderStream {
   private:
    std::unique_ptr<internal::ReaderStream> underlying_stream_;
    std::span<const char> current_span_;
    std::string line_accumulator_;
    std::string current_line_;
    bool is_finished_;
    bool has_pending_line_;
    std::size_t current_line_number_;
    std::size_t start_line_;
    std::size_t end_line_;
    std::size_t initial_line_;
    std::size_t output_position_;
    std::size_t span_pos_;

    enum class ParseResult { HAS_LINE, NEED_MORE_DATA, FINISHED };

    struct DirectOutputResult {
        std::size_t bytes_written;
        bool need_more_data;
        bool finished;
    };

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

    coro::CoroTask<std::span<const char>> read_async() override {
        if (!underlying_stream_ || is_finished_) {
            co_return {};
        }

        while (true) {
            auto result = try_parse_next_line();

            if (result == ParseResult::HAS_LINE) {
                co_return std::span<const char>(current_line_.data(),
                                                current_line_.size());
            }
            if (result == ParseResult::FINISHED) {
                co_return {};
            }

            if (underlying_stream_->done()) {
                if (handle_eof_line()) {
                    co_return std::span<const char>(current_line_.data(),
                                                    current_line_.size());
                }
                is_finished_ = true;
                co_return {};
            }

            current_span_ = co_await underlying_stream_->read_async();
            span_pos_ = 0;
            if (current_span_.empty()) {
                is_finished_ = true;
                co_return {};
            }
        }
    }

    coro::CoroTask<std::size_t> read_async(char* buffer,
                                           std::size_t buffer_size) override {
        if (!underlying_stream_) {
            co_return 0;
        }

        if (has_pending_line_) {
            co_return output_pending_line(buffer, buffer_size);
        }

        if (is_finished_) {
            co_return 0;
        }

        while (true) {
            auto direct = try_direct_output(buffer, buffer_size);
            if (direct.bytes_written > 0) {
                co_return direct.bytes_written;
            }
            if (direct.finished) {
                co_return 0;
            }

            if (!direct.need_more_data) {
                auto result = try_parse_next_line();
                if (result == ParseResult::HAS_LINE) {
                    co_return output_pending_line(buffer, buffer_size);
                }
                if (result == ParseResult::FINISHED) {
                    co_return 0;
                }
            }

            if (underlying_stream_->done()) {
                if (handle_eof_line()) {
                    co_return output_pending_line(buffer, buffer_size);
                }
                is_finished_ = true;
                co_return 0;
            }

            current_span_ = co_await underlying_stream_->read_async();
            span_pos_ = 0;
            if (current_span_.empty()) {
                is_finished_ = true;
                co_return 0;
            }
        }
    }

    bool done() const override { return is_finished_ && !has_pending_line_; }

    void reset() override {
        if (underlying_stream_) {
            underlying_stream_->reset();
        }
        line_accumulator_.clear();
        current_line_.clear();
        current_span_ = std::span<const char>();
        is_finished_ = false;
        has_pending_line_ = false;
        current_line_number_ = initial_line_;
        output_position_ = 0;
        span_pos_ = 0;
    }

   private:
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

    bool has_data_in_span() const { return span_pos_ < current_span_.size(); }

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

    ParseResult try_parse_next_line() {
        if (is_beyond_range()) {
            is_finished_ = true;
            return ParseResult::FINISHED;
        }

        while (has_data_in_span()) {
            const char* newline_ptr = find_next_newline();

            if (!newline_ptr) {
                accumulate_remaining_span();
                return ParseResult::NEED_MORE_DATA;
            }

            std::size_t newline_pos = newline_ptr - current_span_.data();
            if (process_complete_line(newline_pos)) {
                return ParseResult::HAS_LINE;
            }
        }

        return ParseResult::NEED_MORE_DATA;
    }

    DirectOutputResult try_direct_output(char* buffer,
                                         std::size_t buffer_size) {
        if (!line_accumulator_.empty()) {
            return {0, false, false};
        }

        while (true) {
            if (is_beyond_range()) {
                is_finished_ = true;
                return {0, false, true};
            }

            if (!has_data_in_span()) {
                return {0, true, false};
            }

            const char* newline_ptr = find_next_newline();
            if (!newline_ptr) {
                return {0, false, false};
            }

            std::size_t newline_pos = newline_ptr - current_span_.data();
            std::size_t line_length = newline_pos - span_pos_ + 1;

            if (line_length > buffer_size) {
                return {0, false, false};
            }

            bool should_output = should_output_current_line();

            if (is_beyond_range()) {
                is_finished_ = true;
                return {0, false, true};
            }

            current_line_number_++;

            if (should_output) {
                std::memcpy(buffer, current_span_.data() + span_pos_,
                            line_length);
                span_pos_ = newline_pos + 1;
                return {line_length, false, false};
            }

            span_pos_ = newline_pos + 1;
        }
    }

    bool handle_eof_line() {
        if (line_accumulator_.empty()) {
            return false;
        }
        current_line_ = std::move(line_accumulator_);
        line_accumulator_.clear();
        if (should_output_current_line() && !is_beyond_range()) {
            has_pending_line_ = true;
            output_position_ = 0;
            current_line_number_++;
            return true;
        }
        return false;
    }

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

        current_line_.clear();
        return false;
    }

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
