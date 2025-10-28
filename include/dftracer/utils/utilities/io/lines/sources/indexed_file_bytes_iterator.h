#ifndef DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_INDEXED_FILE_BYTES_ITERATOR_H
#define DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_INDEXED_FILE_BYTES_ITERATOR_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/reader/reader.h>
#include <dftracer/utils/reader/stream.h>
#include <dftracer/utils/reader/stream_type.h>
#include <dftracer/utils/utilities/io/lines/iterator.h>
#include <dftracer/utils/utilities/io/lines/line_types.h>

#include <cstring>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

namespace dftracer::utils::utilities::io::lines::sources {

/**
 * @brief Zero-copy iterator for reading lines within byte boundaries from
 * indexed files.
 *
 * Uses StreamType::LINE_BYTES to read lines one at a time within a byte range.
 * The stream handles line boundaries automatically, so no complex buffering
 * is needed.
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
    std::shared_ptr<dftracer::utils::Reader> reader_;
    std::unique_ptr<ReaderStream> stream_;
    std::size_t start_byte_;
    std::size_t end_byte_;
    std::size_t current_line_num_;
    std::size_t buffer_size_;
    mutable std::string line_buffer_;    // Buffer to hold current line data
    mutable std::string stream_buffer_;  // Buffer for reading from stream
    mutable bool stream_done_;
    mutable bool
        has_buffered_line_;        // True if line_buffer_ contains a valid line
    mutable bool attempted_read_;  // True if we've tried to read the first line

   public:
    using Iterator = LineIterator<IndexedFileBytesIterator>;

    /**
     * @brief Construct iterator from existing Reader.
     *
     * @param reader Shared pointer to Reader
     * @param start_byte Starting byte offset (0-based, inclusive)
     * @param end_byte Ending byte offset (0-based, exclusive)
     * @param buffer_size Size of buffer for reading (default 1MB)
     */
    IndexedFileBytesIterator(std::shared_ptr<dftracer::utils::Reader> reader,
                             std::size_t start_byte, std::size_t end_byte,
                             std::size_t buffer_size = 1024 * 1024)
        : reader_(reader),
          start_byte_(start_byte),
          end_byte_(end_byte),
          current_line_num_(1),
          buffer_size_(buffer_size),
          stream_buffer_(),
          stream_done_(false),
          has_buffered_line_(false),
          attempted_read_(false) {
        if (!reader_) {
            throw std::invalid_argument("Reader cannot be null");
        }
        if (start_byte_ >= end_byte_) {
            throw std::invalid_argument("Invalid byte range");
        }

        // Resize (not reserve) to allocate the buffer with the correct size
        stream_buffer_.resize(buffer_size_);

        DFTRACER_UTILS_LOG_DEBUG(
            "IndexedFileBytesIterator: byte range %zu to %zu", start_byte_,
            end_byte_);

        // Create a LINE_BYTES stream for byte-range iteration
        stream_ = reader_->stream(StreamConfig()
                                      .stream_type(StreamType::LINE_BYTES)
                                      .range_type(RangeType::BYTE_RANGE)
                                      .from(start_byte_)
                                      .to(end_byte_));
        if (!stream_) {
            throw std::runtime_error("Failed to create stream");
        }
    }

    /**
     * @brief Check if more lines are available.
     */
    bool has_next() const {
        // Check if we've already determined the stream is done
        if (stream_done_) {
            return false;
        }

        // If we haven't tried to read yet, try to buffer the first line
        if (!attempted_read_) {
            attempted_read_ = true;
            has_buffered_line_ = read_next_line();
            if (!has_buffered_line_) {
                stream_done_ = true;
                return false;
            }
        }

        return has_buffered_line_;
    }

   private:
    /**
     * @brief Read next line from stream into line_buffer_.
     *
     * Since we use LINE_BYTES stream type, each read() returns
     * exactly one complete line (with newline). No complex buffering needed.
     *
     * @return true if a line was read, false if stream is done
     */
    bool read_next_line() const {
        if (stream_->done()) {
            stream_done_ = true;
            return false;
        }

        // LINE_BYTES stream returns one complete line per read()
        std::size_t bytes_read = stream_->read(
            const_cast<char*>(stream_buffer_.data()), stream_buffer_.size());

        if (bytes_read == 0) {
            stream_done_ = true;
            return false;
        }

        // Remove trailing newline if present
        if (bytes_read > 0 && stream_buffer_[bytes_read - 1] == '\n') {
            bytes_read--;
        }

        // Store in line_buffer_ for zero-copy string_view access
        line_buffer_.assign(stream_buffer_.data(), bytes_read);
        return true;
    }

   public:
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

        // Return the buffered line
        Line result(std::string_view(line_buffer_), current_line_num_);
        current_line_num_++;

        // Mark that we need to read the next line on the next has_next() call
        // DON'T read it now, as that would invalidate the string_view we just
        // returned
        has_buffered_line_ = false;
        attempted_read_ = false;

        return result;
    }

    /**
     * @brief Get the current line number.
     */
    std::size_t current_position() const { return current_line_num_; }

    /**
     * @brief Get the underlying Reader.
     */
    std::shared_ptr<dftracer::utils::Reader> get_reader() const {
        return reader_;
    }

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

#endif  // DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_INDEXED_FILE_BYTES_ITERATOR_H
