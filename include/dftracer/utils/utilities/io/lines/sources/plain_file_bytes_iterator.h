#ifndef DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_PLAIN_FILE_BYTES_ITERATOR_H
#define DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_PLAIN_FILE_BYTES_ITERATOR_H

#include <dftracer/utils/utilities/io/lines/iterator.h>
#include <dftracer/utils/utilities/io/lines/line_types.h>

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::io::lines::sources {

class PlainFileBytesIterator {
   private:
    mutable std::ifstream file_;    // mutable for const methods
    std::size_t start_;
    std::size_t end_;
    std::size_t current_line_ = 0;  // Line number of the last returned line
    mutable std::vector<char> line_buffer_;
    mutable bool has_line_ =
        false;                     // true if line_buffer_ contains a valid line
    mutable bool eof_ = false;
    mutable std::size_t pos_ = 0;  // absolute byte position
    mutable bool prefetched_ =
        false;  // true if we already prefetched the next line

    void align_to_next_line() {
        // start_ may land mid-line; skip until \n
        file_.seekg(static_cast<std::streamoff>(start_));
        pos_ = start_;
        if (start_ == 0) return;

        char c;
        while (pos_ < end_ && file_.get(c)) {
            pos_++;
            if (c == '\n') break;
        }
    }

    void prefetch() const {
        if (eof_ || pos_ >= end_) {
            has_line_ = false;
            eof_ = true;
            prefetched_ = true;  // Important: mark as prefetched even when EOF
            return;
        }

        line_buffer_.clear();
        char c;
        bool found_char = false;

        while (pos_ < end_ && file_.get(c)) {
            pos_++;
            found_char = true;
            if (c == '\n') break;
            line_buffer_.push_back(c);
        }

        // Check if we actually read anything
        if (!found_char || (line_buffer_.empty() && pos_ >= end_)) {
            has_line_ = false;
            eof_ = true;
        } else {
            has_line_ = true;
        }
        prefetched_ = true;  // Always mark as prefetched
    }

   public:
    PlainFileBytesIterator(const std::string& path, std::size_t start,
                           std::size_t end)
        : start_(start), end_(end) {
        if (start >= end)
            throw std::invalid_argument("Invalid byte range: start >= end");

        file_.open(path, std::ios::binary);
        if (!file_.is_open())
            throw std::runtime_error("Cannot open file: " + path);

        align_to_next_line();
        // Don't prefetch in constructor - let has_next() do it
    }

    bool has_next() const {
        if (eof_) return false;
        if (!prefetched_) prefetch();
        return has_line_;
    }

    Line next() {
        if (!has_next()) throw std::runtime_error("No more lines available");

        current_line_++;
        prefetched_ = false;  // Next call to has_next() will prefetch
        // Return Line with current buffer content
        // The view is only valid until the next call to has_next() or next()
        return Line(std::string_view(line_buffer_.data(), line_buffer_.size()),
                    current_line_);
    }

    std::size_t current_position() const noexcept { return current_line_; }

    using Iterator = LineIterator<PlainFileBytesIterator>;
    Iterator begin() { return Iterator(this, false); }
    Iterator end() { return Iterator(nullptr, true); }
};

}  // namespace dftracer::utils::utilities::io::lines::sources

#endif  // DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_PLAIN_FILE_BYTES_ITERATOR_H