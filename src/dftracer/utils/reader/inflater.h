#ifndef DFTRACER_UTILS_READER_INFLATER_H
#define DFTRACER_UTILS_READER_INFLATER_H

#include <dftracer/utils/core/common/checkpointer.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/inflater.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/indexer/checkpoint.h>

namespace dftracer::utils {

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
    bool initialize(
        FILE* file, std::uint64_t file_offset = 0,
        int window_bits = constants::indexer::ZLIB_GZIP_WINDOW_BITS) {
        if (!initialize_stream(window_bits)) {
            return false;
        }

        if (fseeko(file, static_cast<off_t>(file_offset), SEEK_SET) != 0) {
            DFTRACER_UTILS_LOG_ERROR("Failed to seek to offset %llu",
                                     file_offset);
            return false;
        }

        return true;
    }

    /**
     * Restore inflater state from a checkpoint for random access
     */
    bool restore_from_checkpoint(FILE* file,
                                 const IndexerCheckpoint& checkpoint) {
        DFTRACER_UTILS_LOG_DEBUG(
            "Restoring from checkpoint: c_offset=%llu, uc_offset=%llu, bits=%d",
            checkpoint.c_offset, checkpoint.uc_offset, checkpoint.bits);

        // Calculate seek position (go back one byte if we have partial bits)
        off_t seek_pos = static_cast<off_t>(checkpoint.c_offset);
        if (checkpoint.bits != 0) {
            seek_pos -= 1;
        }

        if (fseeko(file, seek_pos, SEEK_SET) != 0) {
            DFTRACER_UTILS_LOG_ERROR(
                "Failed to seek to checkpoint position: %lld",
                (long long)seek_pos);
            return false;
        }

        // Reset and initialize with RAW deflate mode
        reset();
        if (!initialize_stream(-15)) {
            DFTRACER_UTILS_LOG_ERROR(
                "Failed to initialize inflater in raw mode", "");
            return false;
        }

        // Decompress and set the dictionary
        unsigned char window[constants::indexer::ZLIB_WINDOW_SIZE];
        std::size_t window_size = constants::indexer::ZLIB_WINDOW_SIZE;

        if (!Checkpointer::decompress(checkpoint.dict_compressed.data(),
                                      checkpoint.dict_compressed.size(), window,
                                      &window_size)) {
            DFTRACER_UTILS_LOG_ERROR(
                "Failed to decompress checkpoint dictionary", "");
            return false;
        }

        if (!set_dictionary(window, window_size)) {
            DFTRACER_UTILS_LOG_ERROR("Failed to set dictionary", "");
            return false;
        }

        // Handle partial byte if necessary
        if (checkpoint.bits != 0) {
            int ch = fgetc(file);
            if (ch == EOF) {
                DFTRACER_UTILS_LOG_ERROR(
                    "Failed to read byte at checkpoint position", "");
                return false;
            }

            int prime_value = ch >> (8 - checkpoint.bits);
            DFTRACER_UTILS_LOG_DEBUG(
                "Applying inflatePrime with %d bits, value: %d (ch=0x%02x)",
                checkpoint.bits, prime_value, ch);

            if (!prime(checkpoint.bits, prime_value)) {
                DFTRACER_UTILS_LOG_ERROR("inflatePrime failed", "");
                return false;
            }
        }

        // Prime with initial input
        if (!read_input(file)) {
            DFTRACER_UTILS_LOG_ERROR(
                "Failed to read initial input after checkpoint restoration",
                "");
            return false;
        }

        DFTRACER_UTILS_LOG_DEBUG("Checkpoint restoration successful", "");
        return true;
    }

    /**
     * Read data continuously (for stream operations)
     */
    bool read(FILE* file, unsigned char* buf, std::size_t len,
              std::size_t& bytes_out) {
        stream.next_out = buf;
        stream.avail_out = static_cast<uInt>(len);
        bytes_out = 0;

        while (stream.avail_out > 0) {
            if (stream.avail_in == 0) {
                if (!read_input(file)) {
                    return false;
                }
                if (stream.avail_in == 0) {
                    break;  // EOF
                }
            }

            int ret = inflate(&stream, Z_NO_FLUSH);

            if (ret == Z_STREAM_END) {
                if (inflateReset(&stream) != Z_OK) {
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
                return false;
            }
        }

        bytes_out = len - stream.avail_out;
        return true;
    }

    /**
     * Skip bytes efficiently by reading and discarding data
     */
    bool skip_bytes(FILE* file, std::size_t bytes_to_skip) {
        DFTRACER_UTILS_LOG_DEBUG(
            "ReaderInflater::skip_bytes - bytes_to_skip=%zu", bytes_to_skip);

        if (bytes_to_skip == 0) return true;

        unsigned char skip_buffer[BUFFER_SIZE];
        std::size_t remaining_skip = bytes_to_skip;
        std::size_t total_skipped = 0;

        while (remaining_skip > 0) {
            std::size_t to_skip = std::min(remaining_skip, sizeof(skip_buffer));
            std::size_t skipped;

            if (!read(file, skip_buffer, to_skip, skipped)) {
                DFTRACER_UTILS_LOG_DEBUG(
                    "Skip failed at total_skipped=%zu, remaining=%zu",
                    total_skipped, remaining_skip);
                return false;
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
        return remaining_skip == 0;
    }

    /**
     * Check if the stream has reached the end
     */
    bool is_at_end() const {
        return stream.avail_in == 0 && stream.avail_out == sizeof(out_buffer);
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_READER_INFLATER_H
