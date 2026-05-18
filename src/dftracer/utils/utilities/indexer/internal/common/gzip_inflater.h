#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_COMMON_GZIP_INFLATER_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_COMMON_GZIP_INFLATER_H

#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/inflater.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/io.h>
#include <fcntl.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dftracer::utils::utilities::indexer::internal {

/**
 * Result structure for GZIP inflater operations
 */
struct GzipInflaterResult {
    std::size_t bytes_read;
    std::uint64_t lines_found;
    bool at_block_boundary;
    std::size_t input_bytes_consumed;
};

/**
 * Common GZIP checkpoint structure used across indexers
 */
struct GzipCheckpoint {
    std::uint64_t checkpoint_idx;
    std::uint64_t uc_offset;
    std::uint64_t uc_size;
    std::uint64_t c_offset;
    std::uint64_t c_size;
    int bits;
    std::vector<unsigned char> dict_compressed;
    std::uint64_t num_lines;
    std::uint64_t first_line_num;
    std::uint64_t last_line_num;
};

/**
 * Inflater specialized for GZIP indexing operations.
 * Handles block boundary detection, line counting, and input byte tracking.
 */
class GzipInflater : public Inflater {
   private:
    std::size_t total_input_bytes_;

   public:
    GzipInflater() : total_input_bytes_(0) {}

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
    coro::CoroTask<bool> read(int fd, off_t& offset, GzipInflaterResult& result,
                              std::size_t max_input_bytes = 0) {
        co_return co_await read_into(fd, offset, out_buffer(), BUFFER_SIZE,
                                     result, max_input_bytes);
    }

    /**
     * Like read() but writes uncompressed output directly into the
     * caller-provided buffer. Enables zero-copy hand-off to downstream
     * consumers that own their own memory (e.g. parallel-inflate worker
     * pools that cycle buffers through a channel without memcpy).
     *
     * The caller must keep `out_buf` alive for the duration of this call
     * and not read it until the coroutine resumes with a successful
     * result.
     */
    coro::CoroTask<bool> read_into(int fd, off_t& offset,
                                   unsigned char* out_buf, std::size_t out_cap,
                                   GzipInflaterResult& result,
                                   std::size_t max_input_bytes = 0) {
        result = {0, 0, false, 0};

        stream.next_out = out_buf;
        stream.avail_out = static_cast<uInt>(out_cap);

        while (stream.avail_out > 0) {
            // Read input if needed
            if (stream.avail_in == 0) {
                std::size_t to_read = BUFFER_SIZE;
                if (max_input_bytes != 0) {
                    if (total_input_bytes_ >= max_input_bytes) break;
                    const std::size_t remaining =
                        max_input_bytes - total_input_bytes_;
                    if (remaining < to_read) to_read = remaining;
                }
                ssize_t n = co_await ::dftracer::utils::io::pread(
                    fd, in_buffer(), to_read, offset);
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
                stream.next_in = in_buffer();
                stream.avail_in = static_cast<uInt>(n);
                total_input_bytes_ += static_cast<std::size_t>(n);
            }

            int ret = inflate(&stream, Z_BLOCK);

            if (ret == Z_STREAM_END) {
                if (inflateReset(&stream) != Z_OK) {
                    DFTRACER_UTILS_LOG_DEBUG(
                        "Failed to reset inflater for next stream: %s",
                        stream.msg ? stream.msg : "no message");
                    break;
                }
                // If the member produced no output (e.g. an FEXTRA-only
                // padding member emitted by the padded-striped writer),
                // keep inflating so the caller never sees a spurious 0-byte
                // read (which it would treat as EOF). Otherwise break so
                // the caller can consume the output and we re-enter with
                // fresh window-accounting post-reset.
                result.at_block_boundary = false;
                if (stream.avail_out < out_cap) break;
                continue;
            }
            if (ret != Z_OK) {
                DFTRACER_UTILS_LOG_DEBUG(
                    "Inflate error during indexing: %d (%s)", ret,
                    stream.msg ? stream.msg : "no message");
                co_return false;
            }

            // Check for proper block boundary (end of header or non-last
            // deflate block).  Stop here so the caller can inspect
            // stream.data_type and create a checkpoint if needed.
            // Only break if we've actually produced output (avail_out <
            // buffer size), otherwise we'd stop at the gzip header boundary
            // before any data is decompressed.
            if ((stream.data_type & 0xc0) == 0x80) {
                result.at_block_boundary = true;
                if (stream.avail_out < out_cap) {
                    break;
                }
            }
        }

        result.bytes_read = out_cap - stream.avail_out;
        result.lines_found = count_lines(out_buf, result.bytes_read);
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
        const unsigned char* p = data;
        const unsigned char* end = data + size;
        while ((p = static_cast<const unsigned char*>(
                    std::memchr(p, '\n', end - p)))) {
            ++lines;
            ++p;
        }
        return lines;
    }
};

}  // namespace dftracer::utils::utilities::indexer::internal

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_COMMON_GZIP_INFLATER_H
