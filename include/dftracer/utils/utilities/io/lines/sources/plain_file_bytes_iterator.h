#ifndef DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_PLAIN_FILE_BYTES_ITERATOR_H
#define DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_PLAIN_FILE_BYTES_ITERATOR_H

#include <dftracer/utils/utilities/io/lines/iterator.h>
#include <dftracer/utils/utilities/io/lines/line_types.h>

#include <fstream>
#include <stdexcept>
#include <string>

namespace dftracer::utils::utilities::io::lines::sources {

/**
 * @brief Zero-copy iterator for reading lines within byte boundaries from plain
 * text files.
 *
 * This iterator seeks to a byte offset and reads lines one at a time within
 * the specified byte range using a single buffer for zero-copy performance.
 *
 * Usage:
 * @code
 * PlainFileBytesIterator it("data.txt", 1000, 5000);  // Bytes 1000-5000
 * while (it.has_next()) {
 *     Line line = it.next();  // Zero-copy string_view
 *     // Use line.content immediately
 * }
 * @endcode
 */
class PlainFileBytesIterator {
   private:
    std::ifstream file_stream_;
    std::size_t end_byte_;
    std::size_t current_byte_;
    std::size_t current_line_num_;
    std::string line_buffer_;  // Single buffer for current line
    bool exhausted_;

   public:
    /**
     * @brief Construct iterator for plain text file.
     *
     * Seeks to the start byte and aligns to line boundaries.
     *
     * @param file_path Path to the plain text file
     * @param start_byte Starting byte offset (0-based, inclusive)
     * @param end_byte Ending byte offset (0-based, exclusive)
     */
    PlainFileBytesIterator(std::string file_path, std::size_t start_byte,
                           std::size_t end_byte)
        : end_byte_(end_byte),
          current_byte_(start_byte),
          current_line_num_(1),
          exhausted_(false) {
        if (start_byte >= end_byte) {
            throw std::invalid_argument("Invalid byte range");
        }

        file_stream_.open(file_path, std::ios::binary);
        if (!file_stream_.is_open()) {
            exhausted_ = true;
            return;
        }

        // Seek to start_byte
        file_stream_.seekg(start_byte);
        current_byte_ = start_byte;

        // If we're not at the beginning, skip to next newline to align to line
        // boundary
        if (start_byte > 0) {
            std::string discard;
            std::getline(file_stream_, discard);
            current_byte_ = file_stream_.tellg();
            if (current_byte_ == static_cast<std::size_t>(-1)) {
                exhausted_ = true;
                return;
            }
        }
    }

    /**
     * @brief Check if more lines are available.
     */
    bool has_next() const {
        return !exhausted_ && current_byte_ < end_byte_ && !file_stream_.eof();
    }

    /**
     * @brief Get the next line (zero-copy).
     *
     * @return Line object with string_view content (valid until next call)
     * @throws std::runtime_error if no more lines available
     */
    Line next() {
        if (!has_next()) {
            throw std::runtime_error("No more lines available");
        }

        // Read one line into buffer
        std::getline(file_stream_, line_buffer_);

        Line result(std::string_view(line_buffer_), current_line_num_);

        // Update position
        std::size_t line_length = line_buffer_.length() + 1;  // +1 for newline
        current_byte_ += line_length;
        current_line_num_++;

        if (file_stream_.eof() || current_byte_ >= end_byte_) {
            exhausted_ = true;
        }

        return result;
    }

    /**
     * @brief Get the current line number.
     */
    std::size_t current_position() const { return current_line_num_; }

    using Iterator = LineIterator<PlainFileBytesIterator>;

    /**
     * @brief Get an iterator to the beginning.
     */
    Iterator begin() { return Iterator(this, false); }

    /**
     * @brief Get an iterator to the end.
     */
    Iterator end() { return Iterator(nullptr, true); }
};

}  // namespace dftracer::utils::utilities::io::lines::sources

#endif  // DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_PLAIN_FILE_BYTES_ITERATOR_H
