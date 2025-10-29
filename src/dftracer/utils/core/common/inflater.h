#ifndef DFTRACER_UTILS_CORE_COMMON_INFLATER_H
#define DFTRACER_UTILS_CORE_COMMON_INFLATER_H

#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace dftracer::utils {

class Inflater {
   public:
    static constexpr std::size_t BUFFER_SIZE = 65536;

    z_stream stream;
    alignas(DFTRACER_OPTIMAL_ALIGNMENT) unsigned char out_buffer[BUFFER_SIZE];
    alignas(DFTRACER_OPTIMAL_ALIGNMENT) unsigned char in_buffer[BUFFER_SIZE];

   protected:
    int window_bits_;

   public:
    Inflater() : window_bits_(constants::indexer::ZLIB_GZIP_WINDOW_BITS) {
        std::memset(&stream, 0, sizeof(stream));
        std::memset(out_buffer, 0, sizeof(out_buffer));
        std::memset(in_buffer, 0, sizeof(in_buffer));
    }

    virtual ~Inflater() { inflateEnd(&stream); }

    bool initialize_stream(int window_bits) {
        window_bits_ = window_bits;
        std::memset(&stream, 0, sizeof(stream));

        if (inflateInit2(&stream, window_bits_) != Z_OK) {
            DFTRACER_UTILS_LOG_ERROR(
                "Failed to initialize inflater with window_bits=%d",
                window_bits_);
            return false;
        }

        stream.avail_in = 0;
        stream.next_in = nullptr;

        return true;
    }

    void reset() {
        inflateEnd(&stream);
        std::memset(&stream, 0, sizeof(stream));
    }

    bool set_dictionary(const unsigned char* dict, std::size_t dict_size) {
        return inflateSetDictionary(&stream, dict,
                                    static_cast<uInt>(dict_size)) == Z_OK;
    }

    bool prime(int bits, int value) {
        return inflatePrime(&stream, bits, value) == Z_OK;
    }

    int detect_stream_type(FILE* file, std::uint64_t offset = 0) {
        if (fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0) {
            return constants::indexer::ZLIB_GZIP_WINDOW_BITS;  // Default to
                                                               // GZIP
        }

        int first_byte = fgetc(file);
        fseeko(file, static_cast<off_t>(offset), SEEK_SET);    // Seek back

        if (first_byte == EOF) {
            return constants::indexer::ZLIB_GZIP_WINDOW_BITS;  // Default to
                                                               // GZIP
        }

        if (first_byte == 0x1f) {
            return constants::indexer::ZLIB_GZIP_WINDOW_BITS;  // GZIP (15+16)
        } else if ((first_byte & 0xf) == 8) {
            return 15;                                         // ZLIB
        } else {
            return -15;                                        // RAW deflate
        }
    }

    bool read_input(FILE* file) {
        std::size_t n = ::fread(in_buffer, 1, sizeof(in_buffer), file);
        if (n > 0) {
            stream.next_in = in_buffer;
            stream.avail_in = static_cast<uInt>(n);
            return true;
        } else if (std::ferror(file)) {
            DFTRACER_UTILS_LOG_DEBUG("Error reading from file: %s",
                                     std::strerror(errno));
            return false;
        }
        return true;  // EOF is not an error
    }

    std::size_t get_output(unsigned char* buf, std::size_t len) {
        std::size_t available = sizeof(out_buffer) - stream.avail_out;
        std::size_t to_copy = std::min(len, available);
        std::memcpy(buf, out_buffer, to_copy);

        // Shift remaining data
        if (to_copy < available) {
            std::memmove(out_buffer, out_buffer + to_copy, available - to_copy);
        }

        return to_copy;
    }

    enum InflateResult {
        SUCCESS,
        END_OF_STREAM,
        ERROR,
        NEED_INPUT,
        NEED_OUTPUT
    };

    InflateResult inflate_chunk(int flush_mode = Z_NO_FLUSH) {
        if (stream.avail_in == 0) {
            return NEED_INPUT;
        }

        stream.next_out = out_buffer;
        stream.avail_out = sizeof(out_buffer);

        int ret = inflate(&stream, flush_mode);

        switch (ret) {
            case Z_OK:
                return SUCCESS;
            case Z_STREAM_END:
                return END_OF_STREAM;
            case Z_BUF_ERROR:
                return stream.avail_in == 0 ? NEED_INPUT : NEED_OUTPUT;
            default:
                DFTRACER_UTILS_LOG_DEBUG(
                    "inflate() failed with error: %d (%s)", ret,
                    stream.msg ? stream.msg : "no message");
                return ERROR;
        }
    }

    bool needs_input() const { return stream.avail_in == 0; }
    bool has_output() const { return stream.avail_out < sizeof(out_buffer); }
    int get_data_type() const { return stream.data_type; }
    std::size_t get_avail_in() const { return stream.avail_in; }
    std::size_t get_avail_out() const { return stream.avail_out; }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_INFLATER_H
