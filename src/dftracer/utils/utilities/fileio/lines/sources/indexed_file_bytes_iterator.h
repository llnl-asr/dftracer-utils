// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_INDEXED_FILE_BYTES_ITERATOR_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_INDEXED_FILE_BYTES_ITERATOR_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/utilities/fileio/lines/iterator.h>
#include <dftracer/utils/utilities/fileio/lines/line_types.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/internal/stream.h>
#include <dftracer/utils/utilities/reader/internal/stream_type.h>

#include <cstring>
#include <iterator>

namespace dftracer::utils::utilities::fileio::lines::sources {

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
 * if (!reader) return reader.error();
 * auto it = IndexedFileBytesIterator::open(*reader, 1000, 5000);  // 1000-5000
 * if (!it) return it.error();
 * while (it->has_next()) {
 *     Line line = it->next();  // Zero-copy string_view
 *     // Use line.content immediately
 * }
 * if (it->error()) return *it->error();  // a failed read ends the loop
 * @endcode
 */
class IndexedFileBytesIterator {
   private:
    dstd::SharedPtr<dftracer::utils::utilities::reader::internal::Reader>
        reader_;
    dstd::UniquePtr<dftracer::utils::utilities::reader::internal::ReaderStream>
        stream_;
    std::size_t current_line_num_;
    std::size_t buffer_size_;
    mutable dstd::String line_buffer_;    ///< Buffer to hold current line data
    mutable dstd::String stream_buffer_;  ///< Buffer for reading from stream
    mutable bool stream_done_;
    mutable bool
        has_buffered_line_;  ///< True if line_buffer_ contains a valid line
    mutable bool
        attempted_read_;     ///< True if we've tried to read the first line
    mutable dstd::Optional<dstd::Error> error_;  ///< The failure that ended it

    IndexedFileBytesIterator(
        dstd::SharedPtr<dftracer::utils::utilities::reader::internal::Reader>
            reader,
        std::size_t buffer_size)
        : reader_(reader),
          current_line_num_(1),
          buffer_size_(buffer_size),
          stream_buffer_(),
          stream_done_(false),
          has_buffered_line_(false),
          attempted_read_(false) {
        // Resize (not reserve) to allocate the buffer with the correct size
        stream_buffer_.resize(buffer_size_);
    }

   public:
    using Iterator = LineIterator<IndexedFileBytesIterator>;

    /**
     * @brief Make an iterator from an existing Reader.
     *
     * @param reader Shared pointer to Reader
     * @param start_byte Starting byte offset (0-based, inclusive)
     * @param end_byte Ending byte offset (0-based, exclusive)
     * @param buffer_size Size of buffer for reading (default 1MB)
     * @return The iterator, or an InvalidArgument error for a null reader or
     *         an empty range, or the error of a stream that cannot be created
     */
    static dstd::Result<IndexedFileBytesIterator> open(
        dstd::SharedPtr<dftracer::utils::utilities::reader::internal::Reader>
            reader,
        std::size_t start_byte, std::size_t end_byte,
        std::size_t buffer_size = 1024 * 1024) {
        if (!reader) {
            return common::make_error(common::ErrorCode::InvalidArgument,
                                      "Reader cannot be null");
        }
        if (start_byte >= end_byte) {
            return common::make_error(common::ErrorCode::InvalidArgument,
                                      "Invalid byte range");
        }

        DFTRACER_UTILS_LOG_DEBUG(
            "IndexedFileBytesIterator: byte range {} to {}", start_byte,
            end_byte);

        IndexedFileBytesIterator iterator(std::move(reader), buffer_size);

        // Create a LINE_BYTES stream for byte-range iteration
        auto opened = iterator.reader_->stream(
            dftracer::utils::utilities::reader::internal::StreamConfig()
                .stream_type(dftracer::utils::utilities::reader::internal::
                                 StreamType::LineBytes)
                .range_type(dftracer::utils::utilities::reader::internal::
                                RangeType::ByteRange)
                .from(start_byte)
                .to(end_byte));
        if (!opened.has_value()) return opened.error();
        iterator.stream_ = std::move(*opened);
        return std::move(iterator);
    }

    /**
     * @brief Check if more lines are available.
     *
     * Returns false when a read fails; @ref error then gives the failure.
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
        auto read = stream_->read(const_cast<char*>(stream_buffer_.data()),
                                  stream_buffer_.size());
        if (!read.has_value()) {
            error_ = read.error();
            stream_done_ = true;
            return false;
        }
        std::size_t bytes_read = *read;

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
     * @return Line object with string_view content (valid until next call).
     *         Call it only after @ref has_next gave true.
     */
    Line next() {
        if (!has_next()) {
            dstd::fatal("IndexedFileBytesIterator::next: no more lines");
        }

        // Return the buffered line
        Line result(dstd::StringView(line_buffer_), current_line_num_);
        current_line_num_++;

        // Mark that we need to read the next line on the next has_next() call
        // DON'T read it now, as that would invalidate the string_view we just
        // returned
        has_buffered_line_ = false;
        attempted_read_ = false;

        return result;
    }

    /**
     * @brief The failure of a stream read that ended the iteration, if any.
     *
     * Check it after the loop: a failed read ends the iteration as a normal
     * end does.
     */
    const dstd::Optional<dstd::Error>& error() const { return error_; }

    /**
     * @brief Get the current line number.
     */
    std::size_t current_position() const { return current_line_num_; }

    /**
     * @brief Get the underlying Reader.
     */
    dstd::SharedPtr<dftracer::utils::utilities::reader::internal::Reader>
    reader() const {
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

}  // namespace dftracer::utils::utilities::fileio::lines::sources

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_LINES_SOURCES_INDEXED_FILE_BYTES_ITERATOR_H
