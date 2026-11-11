// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_FILEIO_TYPES_STREAMING_H
#define DFTRACER_UTILS_FILEIO_TYPES_STREAMING_H

#include <dftracer/utils/dstd/path.h>

namespace dftracer::utils::fileio {

/**
 * @brief Result of a streaming write operation.
 */
struct StreamWriteResult {
    dstd::Path path;
    std::size_t bytes_written = 0;
    std::size_t chunks_written = 0;
    bool success = false;

    StreamWriteResult() = default;

    static StreamWriteResult success_result(dstd::Path p, std::size_t bytes,
                                            std::size_t chunks) {
        StreamWriteResult result;
        result.path = std::move(p);
        result.bytes_written = bytes;
        result.chunks_written = chunks;
        result.success = true;
        return result;
    }
};

}  // namespace dftracer::utils::fileio

#endif  // DFTRACER_UTILS_FILEIO_TYPES_STREAMING_H
