#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_INFLATER_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_INFLATER_H

#include <dftracer/utils/core/common/checkpointer.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/inflater.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/utilities/indexer/internal/checkpoint.h>
#include <fcntl.h>

namespace dftracer::utils::utilities::reader::internal {

/**
 * Inflater specialized for reading operations with checkpoint support.
 * Handles checkpoint restoration, continuous reading, and efficient skipping.
 */
class ReaderInflater : public Inflater {
   public:
    ReaderInflater() {}

    /**
     * Initialize for reading from the beginning of a stream
     */
    coro::CoroTask<bool> initialize(
        int /*fd*/, off_t& offset, std::uint64_t file_offset = 0,
        int window_bits = constants::indexer::ZLIB_GZIP_WINDOW_BITS) {
        if (!initialize_stream(window_bits)) {
            co_return false;
        }

        offset = static_cast<off_t>(file_offset);

        co_return true;
    }

    /**
     * Restore inflater state from a checkpoint for random access
     */
    coro::CoroTask<bool> restore_from_checkpoint(
        int fd, off_t& offset,
        const dftracer::utils::utilities::indexer::internal::IndexerCheckpoint&
            checkpoint) {
        DFTRACER_UTILS_LOG_DEBUG(
            "Restoring from checkpoint: c_offset=%llu, uc_offset=%llu, bits=%d",
            checkpoint.c_offset, checkpoint.uc_offset, checkpoint.bits);

        // Calculate seek position (go back one byte if we have partial bits)
        off_t seek_pos = static_cast<off_t>(checkpoint.c_offset);
        if (checkpoint.bits != 0) {
            seek_pos -= 1;
        }

        offset = seek_pos;

        // Reset and initialize with RAW deflate mode
        reset();
        if (!initialize_stream(-15)) {
            DFTRACER_UTILS_LOG_ERROR(
                "%s", "Failed to initialize inflater in raw mode");
            co_return false;
        }

        // Decompress and set the dictionary
        unsigned char window[constants::indexer::ZLIB_WINDOW_SIZE];
        std::size_t window_size = constants::indexer::ZLIB_WINDOW_SIZE;

        if (!Checkpointer::decompress(checkpoint.dict_compressed.data(),
                                      checkpoint.dict_compressed.size(), window,
                                      &window_size)) {
            DFTRACER_UTILS_LOG_ERROR(
                "%s", "Failed to decompress checkpoint dictionary");
            co_return false;
        }

        if (!set_dictionary(window, window_size)) {
            DFTRACER_UTILS_LOG_ERROR("%s", "Failed to set dictionary");
            co_return false;
        }

        // Handle partial byte if necessary
        if (checkpoint.bits != 0) {
            unsigned char ch;
            ssize_t n =
                co_await ::dftracer::utils::io::pread(fd, &ch, 1, offset);
            if (n <= 0) {
                DFTRACER_UTILS_LOG_ERROR(
                    "%s", "Failed to read byte at checkpoint position");
                co_return false;
            }
            offset += 1;

            int prime_value = ch >> (8 - checkpoint.bits);
            DFTRACER_UTILS_LOG_DEBUG(
                "Applying inflatePrime with %d bits, value: %d (ch=0x%02x)",
                checkpoint.bits, prime_value, ch);

            if (!prime(checkpoint.bits, prime_value)) {
                DFTRACER_UTILS_LOG_ERROR("%s", "inflatePrime failed");
                co_return false;
            }
        }

        // Prime with initial input
        if (!co_await read_input(fd, offset)) {
            DFTRACER_UTILS_LOG_ERROR(
                "%s",
                "Failed to read initial input after checkpoint restoration");
            co_return false;
        }

        DFTRACER_UTILS_LOG_DEBUG("%s", "Checkpoint restoration successful");
        co_return true;
    }

    /**
     * Read data continuously (for stream operations)
     */
    coro::CoroTask<bool> read(int fd, off_t& offset, unsigned char* buf,
                              std::size_t len, std::size_t& bytes_out) {
        stream.next_out = buf;
        stream.avail_out = static_cast<uInt>(len);
        bytes_out = 0;

        while (stream.avail_out > 0) {
            if (stream.avail_in == 0) {
                if (!co_await read_input(fd, offset)) {
                    co_return false;
                }
                if (stream.avail_in == 0) {
                    break;  // EOF
                }
            }

            int ret = inflate(&stream, Z_NO_FLUSH);

            if (ret == Z_STREAM_END) {
                // In raw mode (-15), inflate only processes deflate data;
                // the 8-byte gzip trailer (CRC32 + ISIZE) is NOT consumed.
                // Skip it before switching to gzip auto-detect mode so
                // subsequent members in concatenated gzip are handled.
                if (window_bits_ < 0) {
                    std::size_t trailer = 8;
                    while (trailer > 0) {
                        if (stream.avail_in == 0) {
                            if (!co_await read_input(fd, offset)) {
                                co_return false;
                            }
                            if (stream.avail_in == 0) break;
                        }
                        auto n = std::min(
                            trailer, static_cast<std::size_t>(stream.avail_in));
                        stream.next_in += n;
                        stream.avail_in -= static_cast<uInt>(n);
                        trailer -= n;
                    }
                    if (trailer != 0) {
                        DFTRACER_UTILS_LOG_DEBUG(
                            "Incomplete gzip trailer: %zu bytes remaining",
                            trailer);
                        co_return false;
                    }
                    window_bits_ = constants::indexer::ZLIB_GZIP_WINDOW_BITS;
                }
                if (inflateReset2(&stream,
                                  constants::indexer::ZLIB_GZIP_WINDOW_BITS) !=
                    Z_OK) {
                    DFTRACER_UTILS_LOG_DEBUG(
                        "Failed to reset inflater for next stream: %s",
                        stream.msg ? stream.msg : "no message");
                    break;
                }
                continue;
            }
            if (ret != Z_OK) {
                DFTRACER_UTILS_LOG_DEBUG(
                    "Continuous read failed: %d (%s)", ret,
                    stream.msg ? stream.msg : "no message");
                co_return false;
            }
        }

        bytes_out = len - stream.avail_out;
        co_return true;
    }

    /**
     * Skip bytes efficiently by reading and discarding data
     */
    coro::CoroTask<bool> skip_bytes(int fd, off_t& offset,
                                    std::size_t bytes_to_skip) {
        DFTRACER_UTILS_LOG_DEBUG(
            "ReaderInflater::skip_bytes - bytes_to_skip=%zu", bytes_to_skip);

        if (bytes_to_skip == 0) co_return true;

        unsigned char skip_buffer[BUFFER_SIZE];
        std::size_t remaining_skip = bytes_to_skip;
        std::size_t total_skipped = 0;
        (void)total_skipped;

        while (remaining_skip > 0) {
            std::size_t to_skip = std::min(remaining_skip, sizeof(skip_buffer));
            std::size_t skipped;

            if (!co_await read(fd, offset, skip_buffer, to_skip, skipped)) {
                DFTRACER_UTILS_LOG_DEBUG(
                    "Skip failed at total_skipped=%zu, remaining=%zu",
                    total_skipped, remaining_skip);
                co_return false;
            }

            if (skipped == 0) {
                DFTRACER_UTILS_LOG_DEBUG(
                    "Skip reached EOF at total_skipped=%zu", total_skipped);
                break;
            }

            remaining_skip -= skipped;
            total_skipped += skipped;
        }

        DFTRACER_UTILS_LOG_DEBUG(
            "Skip completed: total_skipped=%zu, success=%s", total_skipped,
            remaining_skip == 0 ? "true" : "false");
        co_return remaining_skip == 0;
    }

    /**
     * Check if the stream has reached the end
     */
    bool is_at_end() const {
        return stream.avail_in == 0 && stream.avail_out == sizeof(out_buffer);
    }
};

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_INFLATER_H
