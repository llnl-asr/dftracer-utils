#ifndef DFTRACER_UTILS_CORE_COMMON_INFLATER_H
#define DFTRACER_UTILS_CORE_COMMON_INFLATER_H

#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/io.h>
#include <fcntl.h>
#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace dftracer::utils {

class Inflater {
   public:
    static constexpr std::size_t BUFFER_SIZE =
        constants::indexer::INFLATE_BUFFER_SIZE;

    z_stream stream;
    alignas(DFTRACER_OPTIMAL_ALIGNMENT) unsigned char out_buffer_[BUFFER_SIZE];
    alignas(DFTRACER_OPTIMAL_ALIGNMENT) unsigned char in_buffer_[BUFFER_SIZE];

    unsigned char* out_buffer() { return out_buffer_; }
    const unsigned char* out_buffer() const { return out_buffer_; }
    unsigned char* in_buffer() { return in_buffer_; }
    const unsigned char* in_buffer() const { return in_buffer_; }

   protected:
    int window_bits_;

   public:
    Inflater() : window_bits_(constants::indexer::ZLIB_GZIP_WINDOW_BITS) {
        std::memset(&stream, 0, sizeof(stream));
        std::memset(out_buffer_, 0, BUFFER_SIZE);
        std::memset(in_buffer_, 0, BUFFER_SIZE);
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

    coro::CoroTask<int> detect_stream_type(int fd,
                                           std::uint64_t file_offset = 0) {
        unsigned char first_byte;
        ssize_t n = co_await io::pread(fd, &first_byte, 1,
                                       static_cast<off_t>(file_offset));
        if (n <= 0) {
            co_return constants::indexer::ZLIB_GZIP_WINDOW_BITS;  // Default
                                                                  // to GZIP
        }

        if (first_byte == constants::indexer::GZIP_MAGIC_BYTE_0) {
            co_return constants::indexer::ZLIB_GZIP_WINDOW_BITS;    // GZIP
        } else if ((first_byte & 0xf) == 8) {
            co_return constants::indexer::ZLIB_FORMAT_WINDOW_BITS;  // ZLIB
        } else {
            co_return constants::indexer::ZLIB_RAW_WINDOW_BITS;  // RAW deflate
        }
    }

    coro::CoroTask<bool> read_input(int fd, off_t& offset) {
        ssize_t n = co_await io::pread(fd, in_buffer(), BUFFER_SIZE, offset);
        if (n > 0) {
            offset += n;
            stream.next_in = in_buffer();
            stream.avail_in = static_cast<uInt>(n);
            co_return true;
        } else if (n < 0) {
            DFTRACER_UTILS_LOG_DEBUG("Error reading from file: %s",
                                     std::strerror(-static_cast<int>(n)));
            co_return false;
        }
        co_return true;  // n == 0 -> EOF, not error
    }

    std::size_t get_output(unsigned char* buf, std::size_t len) {
        std::size_t available = BUFFER_SIZE - stream.avail_out;
        std::size_t to_copy = std::min(len, available);
        std::memcpy(buf, out_buffer(), to_copy);

        // Shift remaining data
        if (to_copy < available) {
            std::memmove(out_buffer(), out_buffer() + to_copy,
                         available - to_copy);
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

        stream.next_out = out_buffer();
        stream.avail_out = BUFFER_SIZE;

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
    bool has_output() const { return stream.avail_out < BUFFER_SIZE; }
    int get_data_type() const { return stream.data_type; }
    std::size_t get_avail_in() const { return stream.avail_in; }
    std::size_t get_avail_out() const { return stream.avail_out; }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_INFLATER_H
