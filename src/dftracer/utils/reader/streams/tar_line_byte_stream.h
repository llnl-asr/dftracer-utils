#ifndef DFTRACER_UTILS_READER_STREAMS_TAR_LINE_BYTE_STREAM_H
#define DFTRACER_UTILS_READER_STREAMS_TAR_LINE_BYTE_STREAM_H

#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/reader/streams/gzip_line_byte_stream.h>
#include <dftracer/utils/reader/streams/tar_stream.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

/**
 * TAR-aware line byte stream that provides logical line-based reading from
 * TAR.GZ files. Presents concatenated file contents without TAR headers,
 * ensuring complete lines.
 */
class TarLineByteStream : public TarStream {
   private:
    TarIndexer* tar_indexer_ptr_;
    std::unique_ptr<GzipLineByteStream> current_file_stream_;
    std::string
        line_buffer_;  // Buffer for accumulating partial lines across files

   public:
    TarLineByteStream() : TarStream(), tar_indexer_ptr_(nullptr) {}

    void initialize(const std::string& tar_gz_path, std::size_t start_bytes,
                    std::size_t end_bytes,
                    dftracer::utils::Indexer& indexer) override {
        DFTRACER_UTILS_LOG_DEBUG(
            "TarLineByteStream::initialize - start_bytes=%zu, end_bytes=%zu",
            start_bytes, end_bytes);

        // Cast to TarIndexer for TAR-specific operations
        tar_indexer_ptr_ = dynamic_cast<TarIndexer*>(&indexer);
        if (!tar_indexer_ptr_) {
            throw ReaderError(ReaderError::INITIALIZATION_ERROR,
                              "TarLineByteStream requires a TarIndexer");
        }

        TarStream::initialize(tar_gz_path, start_bytes, end_bytes, indexer);
        current_position_ = start_bytes;
        line_buffer_.clear();

        // Initialize stream for current file if we have one
        if (current_file_) {
            initialize_current_file_stream();
        }

        DFTRACER_UTILS_LOG_DEBUG(
            "TarLineByteStream::initialize - completed, current_position_=%zu",
            current_position_);
    }

    std::size_t read(char* buffer, std::size_t buffer_size) override {
#ifdef __GNUC__
        __builtin_prefetch(buffer, 1, 3);
#endif

        if (!decompression_initialized_) {
            throw ReaderError(
                ReaderError::INITIALIZATION_ERROR,
                "TAR line streaming session not properly initialized");
        }

        if (is_at_target_end()) {
            is_finished_ = true;
            return 0;
        }

        std::size_t total_bytes_written = 0;
        std::size_t remaining_buffer = buffer_size;

        // First, check if we have buffered data from previous calls
        if (!line_buffer_.empty()) {
            std::size_t buffer_bytes =
                std::min(line_buffer_.size(), remaining_buffer);
            std::memcpy(buffer, line_buffer_.c_str(), buffer_bytes);

            if (buffer_bytes < line_buffer_.size()) {
                // Partial consumption of buffer
                line_buffer_ = line_buffer_.substr(buffer_bytes);
                return buffer_bytes;
            } else {
                // Full consumption of buffer
                line_buffer_.clear();
                total_bytes_written += buffer_bytes;
                remaining_buffer -= buffer_bytes;
            }
        }

        // Read complete lines from current and subsequent files
        std::vector<char> temp_buffer(remaining_buffer);

        while (remaining_buffer > 0 && current_position_ < target_end_bytes_ &&
               current_file_) {
            // Calculate how much we can read from current file
            std::size_t file_remaining =
                current_file_->file_size - current_file_offset_;
            std::size_t logical_remaining =
                target_end_bytes_ - current_position_;
            std::size_t max_read = std::min(
                remaining_buffer, std::min(file_remaining, logical_remaining));

            if (max_read == 0) {
                // Move to next file
                if (!advance_to_next_file()) {
                    break;
                }
                initialize_current_file_stream();
                continue;
            }

            // Read from current file stream
            if (!current_file_stream_) {
                initialize_current_file_stream();
                if (!current_file_stream_) {
                    break;
                }
            }

            std::size_t bytes_read =
                current_file_stream_->read(temp_buffer.data(), max_read);

            DFTRACER_UTILS_LOG_DEBUG(
                "TarLineByteStream::stream - read %zu bytes from file %s "
                "(offset %zu)",
                bytes_read, current_file_->file_name.c_str(),
                current_file_offset_);

            if (bytes_read == 0) {
                // Current file stream is finished, move to next file
                if (!advance_to_next_file()) {
                    break;
                }
                initialize_current_file_stream();
                continue;
            }

            // Process the data to ensure complete lines
            std::size_t processed = process_line_data(
                temp_buffer.data(), bytes_read, buffer + total_bytes_written,
                remaining_buffer);

            total_bytes_written += processed;
            remaining_buffer -= processed;
            current_position_ += bytes_read;
            current_file_offset_ += bytes_read;

            // Check if we've finished the current file
            if (current_file_offset_ >= current_file_->file_size) {
                if (!advance_to_next_file()) {
                    break;
                }
                initialize_current_file_stream();
            }

            // If we couldn't process all data, we're likely at a buffer
            // boundary
            if (processed == 0 && bytes_read > 0) {
                break;
            }
        }

        if (total_bytes_written == 0 ||
            current_position_ >= target_end_bytes_) {
            is_finished_ = true;
        }

        DFTRACER_UTILS_LOG_DEBUG(
            "TarLineByteStream::stream - total bytes written: %zu (position: "
            "%zu / %zu)",
            total_bytes_written, current_position_, target_end_bytes_);

        return total_bytes_written;
    }

    void reset() override {
        TarStream::reset();
        current_file_stream_.reset();
        tar_indexer_ptr_ = nullptr;
        line_buffer_.clear();
    }

   private:
    void initialize_current_file_stream() {
        current_file_stream_.reset();

        if (!current_file_ || !tar_indexer_ptr_) {
            return;
        }

        // Get the actual file data offset from the TAR indexer
        TarIndexer::TarFileInfo tar_file_info;
        if (!tar_indexer_ptr_->find_file(current_file_->file_name,
                                         tar_file_info)) {
            DFTRACER_UTILS_LOG_ERROR("Failed to find TAR file: %s",
                                     current_file_->file_name.c_str());
            return;
        }

        // Calculate actual byte range for this file segment
        std::uint64_t actual_start =
            tar_file_info.data_offset + current_file_offset_;
        std::uint64_t actual_end =
            tar_file_info.data_offset + current_file_->file_size;

        // Clamp to our target range
        std::uint64_t logical_start =
            current_file_->logical_start_offset + current_file_offset_;
        std::uint64_t logical_end =
            std::min(static_cast<std::uint64_t>(target_end_bytes_),
                     current_file_->logical_end_offset);

        if (logical_start >= logical_end) {
            return;
        }

        // Adjust actual end based on logical constraint
        std::uint64_t logical_size = logical_end - logical_start;
        actual_end = actual_start + logical_size;

        DFTRACER_UTILS_LOG_DEBUG(
            "Initializing line stream for file %s: actual[%lu-%lu] "
            "logical[%lu-%lu]",
            current_file_->file_name.c_str(), actual_start, actual_end,
            logical_start, logical_end);

        // Create and initialize the underlying line byte stream
        current_file_stream_ = std::make_unique<GzipLineByteStream>();
        current_file_stream_->initialize(
            current_gz_path_, static_cast<std::size_t>(actual_start),
            static_cast<std::size_t>(actual_end), *tar_indexer_ptr_);
    }

    /**
     * Process raw data to extract complete lines, buffering partial lines
     * Returns number of bytes written to output buffer
     */
    std::size_t process_line_data(const char* input_data,
                                  std::size_t input_size, char* output_buffer,
                                  std::size_t output_size) {
        std::size_t bytes_written = 0;

        // Accumulate input data with any existing line buffer
        std::string working_buffer = line_buffer_;
        working_buffer.append(input_data, input_size);
        line_buffer_.clear();

        // Extract complete lines
        std::size_t search_pos = 0;
        while (search_pos < working_buffer.size() &&
               bytes_written < output_size) {
            std::size_t newline_pos = working_buffer.find('\n', search_pos);

            if (newline_pos == std::string::npos) {
                // No complete line found, buffer the remainder
                line_buffer_ = working_buffer.substr(search_pos);
                break;
            }

            // Include the newline in the line
            std::size_t line_size = newline_pos - search_pos + 1;

            if (bytes_written + line_size > output_size) {
                // Line doesn't fit in output buffer, buffer it
                line_buffer_ = working_buffer.substr(search_pos);
                break;
            }

            // Copy complete line to output
            std::memcpy(output_buffer + bytes_written,
                        working_buffer.c_str() + search_pos, line_size);
            bytes_written += line_size;
            search_pos = newline_pos + 1;
        }

        return bytes_written;
    }
};

#endif  // DFTRACER_UTILS_READER_STREAMS_TAR_LINE_BYTE_STREAM_H
