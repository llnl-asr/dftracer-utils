#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_INFLATER_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_INFLATER_H

#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/inflater.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/io.h>
#include <fcntl.h>

namespace dftracer::utils::utilities::indexer::internal {

/**
 * Result structure for indexer inflater operations
 */
struct IndexerInflaterResult {
    std::size_t bytes_read;
    std::uint64_t lines_found;
    bool at_block_boundary;
    std::size_t input_bytes_consumed;
};

/**
 * Inflater specialized for indexing operations.
 * Handles block boundary detection, line counting, and input byte tracking.
 */
class IndexerInflater : public Inflater {
   private:
    std::size_t total_input_bytes_;

   public:
    IndexerInflater() : total_input_bytes_(0) {}

    /**
     * Initialize for indexing with auto-detection or specified window bits
     */
    coro::CoroTask<bool> initialize(int fd, std::uint64_t file_offset = 0,
                                    int window_bits = 0) {
        if (window_bits == 0) {
            window_bits = co_await detect_stream_type(fd, file_offset);
        }

        if (!initialize_stream(window_bits)) {
            co_return false;
        }

        total_input_bytes_ = 0;
        co_return true;
    }

    /**
     * Read and analyze data for indexing purposes.
     * Uses Z_BLOCK to detect deflate boundaries and counts lines.
     */
    coro::CoroTask<bool> read(int fd, off_t& offset,
                              IndexerInflaterResult& result) {
        result = {0, 0, false, 0};

        stream.next_out = out_buffer;
        stream.avail_out = sizeof(out_buffer);

        while (stream.avail_out > 0) {
            // Read input if needed
            if (stream.avail_in == 0) {
                ssize_t n = co_await ::dftracer::utils::io::pread(
                    fd, in_buffer, sizeof(in_buffer), offset);
                if (n == 0) {
                    break;  // EOF
                }
                if (n < 0) {
                    DFTRACER_UTILS_LOG_DEBUG(
                        "File read error during indexing: %s",
                        std::strerror(-static_cast<int>(n)));
                    co_return false;  // Return error
                }
                offset += n;
                stream.next_in = in_buffer;
                stream.avail_in = static_cast<uInt>(n);
                total_input_bytes_ += static_cast<std::size_t>(n);
            }

            int ret = inflate(&stream, Z_BLOCK);

            if (ret == Z_STREAM_END) {
                break;
            }
            if (ret != Z_OK) {
                DFTRACER_UTILS_LOG_DEBUG(
                    "Inflate error during indexing: %d (%s)", ret,
                    stream.msg ? stream.msg : "no message");
                co_return false;
            }

            // Check for proper block boundary (end of header or non-last
            // deflate block)
            if ((stream.data_type & 0xc0) == 0x80) {
                result.at_block_boundary = true;
                // Continue processing - don't break immediately
            }
        }

        result.bytes_read = sizeof(out_buffer) - stream.avail_out;
        result.lines_found = count_lines(out_buffer, result.bytes_read);
        result.input_bytes_consumed = total_input_bytes_ - stream.avail_in;

        co_return true;
    }

    /**
     * Check if currently at a valid checkpoint boundary
     */
    bool is_at_checkpoint_boundary() const {
        return (stream.data_type & 0xc0) == 0x80;
    }

    /**
     * Get total input bytes consumed from the stream
     */
    std::size_t get_total_input_consumed() const {
        return total_input_bytes_ - stream.avail_in;
    }

    /**
     * Reset the input byte counter (useful when restarting from a checkpoint)
     */
    void reset_input_counter() { total_input_bytes_ = 0; }

   private:
    /**
     * Count newlines in the given data buffer
     */
    std::uint64_t count_lines(const unsigned char* data,
                              std::size_t size) const {
        std::uint64_t lines = 0;
        for (std::size_t i = 0; i < size; i++) {
            if (data[i] == '\n') {
                lines++;
            }
        }
        return lines;
    }
};

}  // namespace dftracer::utils::utilities::indexer::internal

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_INFLATER_H
