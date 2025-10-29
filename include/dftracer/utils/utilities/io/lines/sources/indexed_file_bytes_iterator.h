#ifndef DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_INDEXED_FILE_BYTES_ITERATOR_H
#define DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_INDEXED_FILE_BYTES_ITERATOR_H

#include <dftracer/utils/reader/reader.h>
#include <dftracer/utils/utilities/io/lines/line_types.h>

#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace dftracer::utils::utilities::io::lines::sources {

/**
 * @brief Zero-copy iterator for reading lines within byte boundaries from
 * indexed files.
 *
 * Reads the entire byte range once, then iterates through lines one at a time
 * using a single buffer for zero-copy performance.
 *
 * Usage:
 * @code
 * auto reader = ReaderFactory::create("file.gz", "file.gz.idx");
 * IndexedFileBytesIterator it(reader, 1000, 5000);  // Bytes 1000-5000
 * while (it.has_next()) {
 *     Line line = it.next();  // Zero-copy string_view
 *     // Use line.content immediately
 * }
 * @endcode
 */
class IndexedFileBytesIterator {
   private:
    std::string all_content_;    // All content in byte range
    std::istringstream stream_;  // Stream to read lines from content
    std::size_t current_line_num_;
    std::string line_buffer_;    // Single buffer for current line
    bool exhausted_;

   public:
    /**
     * @brief Construct iterator from existing Reader.
     *
     * @param reader Shared pointer to Reader
     * @param start_byte Starting byte offset (0-based, inclusive)
     * @param end_byte Ending byte offset (0-based, exclusive)
     */
    IndexedFileBytesIterator(std::shared_ptr<dftracer::utils::Reader> reader,
                             std::size_t start_byte, std::size_t end_byte)
        : current_line_num_(1), exhausted_(false) {
        if (!reader) {
            throw std::invalid_argument("Reader cannot be null");
        }
        if (start_byte >= end_byte) {
            throw std::invalid_argument("Invalid byte range");
        }

        // Read all content in the byte range at once
        std::vector<char> buffer(end_byte - start_byte);
        std::size_t bytes_read = reader->read_line_bytes(
            start_byte, end_byte, buffer.data(), buffer.size());

        if (bytes_read > 0) {
            all_content_.assign(buffer.data(), bytes_read);
            stream_.str(all_content_);
        } else {
            exhausted_ = true;
        }
    }

    /**
     * @brief Check if more lines are available.
     */
    bool has_next() const { return !exhausted_ && !stream_.eof(); }

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

        // Read one line from the stream
        if (!std::getline(stream_, line_buffer_)) {
            exhausted_ = true;
            throw std::runtime_error("Failed to read next line");
        }

        Line result(std::string_view(line_buffer_), current_line_num_);
        current_line_num_++;

        return result;
    }

    /**
     * @brief Get the current line number.
     */
    std::size_t current_position() const { return current_line_num_; }
};

}  // namespace dftracer::utils::utilities::io::lines::sources

#endif  // DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_INDEXED_FILE_BYTES_ITERATOR_H
