#ifndef DFTRACER_UTILS_INDEXER_INFLATER_H
#define DFTRACER_UTILS_INDEXER_INFLATER_H

#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/inflater.h>
#include <dftracer/utils/core/common/logging.h>

namespace dftracer::utils {

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
    bool initialize(FILE* file, std::uint64_t file_offset = 0,
                    int window_bits = 0) {
        if (window_bits == 0) {
            window_bits = detect_stream_type(file, file_offset);
        }

        if (!initialize_stream(window_bits)) {
            return false;
        }

        // Seek to starting position
        if (fseeko(file, static_cast<off_t>(file_offset), SEEK_SET) != 0) {
            DFTRACER_UTILS_LOG_ERROR("Failed to seek to offset %llu",
                                     file_offset);
            return false;
        }

        total_input_bytes_ = 0;
        return true;
    }

    /**
     * Read and analyze data for indexing purposes.
     * Uses Z_BLOCK to detect deflate boundaries and counts lines.
     */
    bool read(FILE* file, IndexerInflaterResult& result) {
        result = {0, 0, false, 0};

        stream.next_out = out_buffer;
        stream.avail_out = sizeof(out_buffer);

        while (stream.avail_out > 0) {
            // Read input if needed
            if (stream.avail_in == 0) {
                std::size_t n = ::fread(in_buffer, 1, sizeof(in_buffer), file);
                if (n == 0) {
                    if (std::ferror(file)) {
                        DFTRACER_UTILS_LOG_DEBUG(
                            "File read error during indexing: %s",
                            std::strerror(errno));
                        return false;  // Return error
                    }
                    break;             // EOF
                }
                stream.next_in = in_buffer;
                stream.avail_in = static_cast<uInt>(n);
                total_input_bytes_ += n;
            }

            int ret = inflate(&stream, Z_BLOCK);

            if (ret == Z_STREAM_END) {
                break;
            }
            if (ret != Z_OK) {
                DFTRACER_UTILS_LOG_DEBUG(
                    "Inflate error during indexing: %d (%s)", ret,
                    stream.msg ? stream.msg : "no message");
                return false;
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

        return true;
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

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_INDEXER_INFLATER_H
