#ifndef DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_INDEXED_FILE_LINE_ITERATOR_H
#define DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_INDEXED_FILE_LINE_ITERATOR_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/reader/reader.h>
#include <dftracer/utils/reader/reader_factory.h>
#include <dftracer/utils/reader/stream.h>
#include <dftracer/utils/reader/stream_type.h>
#include <dftracer/utils/utilities/io/lines/iterator.h>
#include <dftracer/utils/utilities/io/lines/line_types.h>

#include <cstring>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::io::lines::sources {

/**
 * @brief Configuration for IndexedFileLineIterator.
 *
 * The iterator automatically selects the appropriate stream type:
 * - LINE_RANGE: Uses StreamType::LINE
 * - BYTE_RANGE: Uses StreamType::LINE_BYTES
 *
 * Usage:
 * @code
 * // Option 1: From file path
 * auto config = IndexedFileLineIteratorConfig()
 *     .with_file("file.gz", "file.gz.idx")
 *     .with_line_range(1, 100)
 *     .with_buffer_size(128 * 1024);
 *
 * // Option 2: From existing reader
 * auto config = IndexedFileLineIteratorConfig()
 *     .with_reader(reader)
 *     .with_line_range(1, 100);
 *
 * IndexedFileLineIterator iter(config);
 * @endcode
 */
class IndexedFileLineIteratorConfig {
   private:
    std::shared_ptr<dftracer::utils::Reader> reader_;
    RangeType range_type_ = RangeType::LINE_RANGE;
    std::size_t start_ = 1;
    std::size_t end_ = 0;
    std::size_t buffer_size_ = 1024 * 1024;  // 1MB default buffer

   public:
    IndexedFileLineIteratorConfig() = default;

    IndexedFileLineIteratorConfig& with_file(
        const std::string& file_path, const std::string& index_path = "") {
        reader_ = dftracer::utils::ReaderFactory::create(file_path, index_path);
        return *this;
    }

    IndexedFileLineIteratorConfig& with_reader(
        std::shared_ptr<dftracer::utils::Reader> reader) {
        reader_ = reader;
        return *this;
    }

    IndexedFileLineIteratorConfig& with_range_type(RangeType type) {
        range_type_ = type;
        return *this;
    }

    IndexedFileLineIteratorConfig& with_line_range(std::size_t start_line,
                                                   std::size_t end_line) {
        range_type_ = RangeType::LINE_RANGE;
        start_ = start_line;
        end_ = end_line;
        return *this;
    }

    IndexedFileLineIteratorConfig& with_byte_range(std::size_t start_bytes,
                                                   std::size_t end_bytes) {
        range_type_ = RangeType::BYTE_RANGE;
        start_ = start_bytes;
        end_ = end_bytes;
        return *this;
    }

    IndexedFileLineIteratorConfig& with_buffer_size(std::size_t size) {
        buffer_size_ = size;
        return *this;
    }

    std::shared_ptr<dftracer::utils::Reader> reader() const { return reader_; }
    RangeType range_type() const { return range_type_; }
    std::size_t start() const { return start_; }
    std::size_t end() const { return end_; }
    std::size_t buffer_size() const { return buffer_size_; }
};

/**
 * @brief Unified zero-copy line iterator over indexed archive files.
 *
 * This iterator returns one line at a time. It automatically selects the
 * appropriate stream type based on the range:
 * - LINE_RANGE: Uses StreamType::LINE (line-number based seeking)
 * - BYTE_RANGE: Uses StreamType::LINE_BYTES (byte-offset based seeking)
 *
 * Both stream types return one complete line per read(), so no complex
 * buffering or newline splitting is needed.
 *
 * Usage:
 * @code
 * auto reader = ReaderFactory::create("file.gz", "file.gz.idx");
 *
 * auto config = IndexedFileLineIteratorConfig()
 *     .with_reader(reader)
 *     .with_line_range(1, 100);
 * IndexedFileLineIterator lines(config);
 *
 * // Iterate
 * for (const auto& line : lines) {
 *     std::cout << line.content << "\n";
 * }
 * @endcode
 */
class IndexedFileLineIterator {
   private:
    IndexedFileLineIteratorConfig config_;
    std::unique_ptr<ReaderStream> stream_;
    std::size_t current_position_;
    mutable std::string line_buffer_;    // Buffer to hold current line data
    mutable std::string stream_buffer_;  // Buffer for reading from stream
    mutable bool stream_done_;
    mutable bool
        has_buffered_line_;        // True if line_buffer_ contains a valid line
    mutable bool attempted_read_;  // True if we've tried to read the first line

   public:
    using Iterator = LineIterator<IndexedFileLineIterator>;

    /**
     * @brief Construct iterator from config object.
     *
     * @param config Configuration object with builder pattern
     */
    explicit IndexedFileLineIterator(
        const IndexedFileLineIteratorConfig& config)
        : config_(config),
          current_position_(config.start()),
          stream_buffer_(),  // Initialize empty
          stream_done_(false),
          has_buffered_line_(false),
          attempted_read_(false) {
        // Resize (not reserve) to allocate the buffer with the correct size
        // This gives us a buffer of the right size without filling it with '\0'
        stream_buffer_.resize(config.buffer_size());
        validate_and_initialize();
    }

   private:
    void validate_and_initialize() {
        if (!config_.reader()) {
            throw std::invalid_argument("Reader cannot be null");
        }

        // Validate range based on type
        if (config_.range_type() == RangeType::LINE_RANGE) {
            if (config_.end() == 0) {
                std::size_t num_lines = config_.reader()->get_num_lines();
                DFTRACER_UTILS_LOG_DEBUG(
                    "IndexedFileLineIterator: end=0, setting to num_lines=%zu",
                    num_lines);
                config_ = config_.with_line_range(config_.start(), num_lines);
            }

            DFTRACER_UTILS_LOG_DEBUG(
                "IndexedFileLineIterator: line range %zu to %zu",
                config_.start(), config_.end());

            if (config_.start() < 1 || config_.end() < config_.start()) {
                throw std::invalid_argument(
                    "Invalid line range (must be 1-based and start <= end)");
            }
        } else {
            if (config_.end() < config_.start()) {
                throw std::invalid_argument(
                    "Invalid byte range (start <= end)");
            }
        }

        StreamType stream_type = (config_.range_type() == RangeType::LINE_RANGE)
                                     ? StreamType::LINE
                                     : StreamType::LINE_BYTES;

        stream_ = config_.reader()->stream(stream_type, config_.range_type(),
                                           config_.start(), config_.end());
        if (!stream_) {
            throw std::runtime_error("Failed to create stream");
        }
    }

   public:
    /**
     * @brief Check if more lines are available.
     */
    bool has_next() const {
        // Check if we've already determined the stream is done
        if (stream_done_) {
            return false;
        }
        // Check if we've exceeded our requested range
        if (current_position_ > config_.end()) {
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
     * Since we use LINE or LINE_BYTES stream types, each read() returns
     * exactly one complete line (with newline). No complex buffering needed.
     *
     * @return true if a line was read, false if stream is done
     */
    bool read_next_line() const {
        if (stream_->done()) {
            stream_done_ = true;
            return false;
        }

        // LINE and LINE_BYTES streams return one complete line per read()
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
        Line result(std::string_view(line_buffer_), current_position_);
        current_position_++;

        // Mark that we need to read the next line on the next has_next() call
        // DON'T read it now, as that would invalidate the string_view we just
        // returned
        has_buffered_line_ = false;
        attempted_read_ = false;

        return result;
    }

    /**
     * @brief Get the current position.
     *
     * Interpretation depends on range_type:
     * - LINE_RANGE: Current line number (1-based)
     * - BYTE_RANGE: Current byte offset
     */
    std::size_t current_position() const { return current_position_; }

    /**
     * @brief Get total count in this range.
     *
     * Interpretation depends on range_type:
     * - LINE_RANGE: Number of lines
     * - BYTE_RANGE: Number of bytes
     */
    std::size_t total_count() const {
        return (config_.end() >= config_.start())
                   ? (config_.end() - config_.start() + 1)
                   : 0;
    }

    /**
     * @brief Get the range type being used.
     */
    RangeType range_type() const { return config_.range_type(); }

    /**
     * @brief Get the underlying Reader.
     */
    std::shared_ptr<dftracer::utils::Reader> get_reader() const {
        return config_.reader();
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

#endif  // DFTRACER_UTILS_UTILITIES_IO_LINES_SOURCES_INDEXED_FILE_LINE_ITERATOR_H
