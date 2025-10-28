#ifndef DFTRACER_UTILS_INDEXER_COMMON_GZIP_CHECKPOINTER_H
#define DFTRACER_UTILS_INDEXER_COMMON_GZIP_CHECKPOINTER_H

#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/indexer/common/gzip_inflater.h>
#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dftracer::utils {

/**
 * GZIP checkpointer for creating and managing checkpoint data
 */
class GzipCheckpointer {
   public:
    std::size_t uc_offset;
    std::size_t c_offset;
    int bits;
    GzipInflater& inflater;
    alignas(DFTRACER_OPTIMAL_ALIGNMENT) unsigned char window
        [constants::indexer::ZLIB_WINDOW_SIZE];

    GzipCheckpointer(GzipInflater& in, std::size_t uc_offset_ = 0)
        : uc_offset(uc_offset_), c_offset(0), bits(0), inflater(in) {
        std::memset(window, 0, sizeof(window));
    }

    bool create() { return create(c_offset); }

    bool create(std::size_t compressed_offset) {
        // Use the provided compressed offset (calculated correctly by caller)
        c_offset = compressed_offset;

        // Get bit offset from zlib state (following zran approach)
        bits = inflater.stream.data_type & 7;

        // Try to get the sliding window dictionary from zlib
        // This contains the last 32KB of uncompressed data
        unsigned have = 0;

        // Check if we're at proper deflate block boundary (end of header or
        // non-last block) Following zran.c logic: (data_type & 0xc0) == 0x80
        if ((inflater.stream.data_type & 0xc0) != 0x80) {
            DFTRACER_UTILS_LOG_DEBUG(
                "Cannot create checkpoint: not at proper deflate block "
                "boundary (data_type=0x%x)",
                inflater.stream.data_type);
            return false;
        }

        if (inflateGetDictionary(&inflater.stream, window, &have) != Z_OK) {
            DFTRACER_UTILS_LOG_DEBUG(
                "Could not get dictionary for checkpoint at offset %zu "
                "(data_type=0x%x)",
                uc_offset, inflater.stream.data_type);
            return false;
        }

        // If less than 32KB available, right-align and pad with zeros
        if (have < constants::indexer::ZLIB_WINDOW_SIZE) {
            std::memmove(window + (constants::indexer::ZLIB_WINDOW_SIZE - have),
                         window, have);
            std::memset(window, 0, constants::indexer::ZLIB_WINDOW_SIZE - have);
        }

        DFTRACER_UTILS_LOG_DEBUG(
            "Created checkpoint: uc_offset=%zu, c_offset=%zu, bits=%d, "
            "dict_size=%u, data_type=0x%x",
            uc_offset, c_offset, bits, have, inflater.stream.data_type);
        return true;
    }

    bool compress(std::vector<unsigned char>& compressed_data) const {
        z_stream zs;
        std::memset(&zs, 0, sizeof(zs));

        if (deflateInit(&zs, Z_BEST_COMPRESSION) != Z_OK) {
            DFTRACER_UTILS_LOG_DEBUG("%s", "Failed to initialize zlib");
            return false;
        }

        size_t max_compressed =
            deflateBound(&zs, constants::indexer::ZLIB_WINDOW_SIZE);
        compressed_data.resize(max_compressed);

        zs.next_in = const_cast<unsigned char*>(window);
        zs.avail_in = static_cast<uInt>(constants::indexer::ZLIB_WINDOW_SIZE);
        zs.next_out = compressed_data.data();
        zs.avail_out = static_cast<uInt>(max_compressed);

        int ret = deflate(&zs, Z_FINISH);
        if (ret != Z_STREAM_END) {
            DFTRACER_UTILS_LOG_ERROR(
                "Failed to compress dictionary data with error: %d (%s)", ret,
                zs.msg ? zs.msg : "no message");
            deflateEnd(&zs);
            return false;
        }

        compressed_data.resize(max_compressed - zs.avail_out);
        deflateEnd(&zs);
        return true;
    }

    static bool decompress(const std::vector<unsigned char>& compressed,
                           unsigned char* window, std::size_t* window_size) {
        return decompress(compressed.data(), compressed.size(), window,
                          window_size);
    }

    static bool decompress(const unsigned char* compressed,
                           std::size_t compressed_size, unsigned char* window,
                           std::size_t* window_size) {
        z_stream zs;
        std::memset(&zs, 0, sizeof(zs));

        if (inflateInit(&zs) != Z_OK) {
            DFTRACER_UTILS_LOG_DEBUG("%s", "Failed to initialize zlib");
            return false;
        }

        zs.next_in = const_cast<unsigned char*>(compressed);
        zs.avail_in = static_cast<uInt>(compressed_size);
        zs.next_out = window;
        zs.avail_out = static_cast<uInt>(*window_size);

        int ret = inflate(&zs, Z_FINISH);
        if (ret != Z_STREAM_END) {
            DFTRACER_UTILS_LOG_ERROR(
                "inflate failed during window decompression with error: %d "
                "(%s)",
                ret, zs.msg ? zs.msg : "no message");
            inflateEnd(&zs);
            return false;
        }

        *window_size = *window_size - zs.avail_out;
        inflateEnd(&zs);
        return true;
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_INDEXER_COMMON_GZIP_CHECKPOINTER_H
