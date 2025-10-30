#ifndef DFTRACER_UTILS_UTILITIES_IO_STREAMING_H
#define DFTRACER_UTILS_UTILITIES_IO_STREAMING_H

#include <dftracer/utils/utilities/io/types/types.h>

#include <functional>
#include <vector>

namespace dftracer::utils::utilities::io {

/**
 * @brief Configuration for streaming file read operations.
 */
struct StreamReadInput {
    fs::path path;
    std::size_t chunk_size = 64 * 1024;  // 64KB default

    StreamReadInput() = default;

    explicit StreamReadInput(fs::path p, std::size_t cs = 64 * 1024)
        : path(std::move(p)), chunk_size(cs) {}

    // Equality for caching support
    bool operator==(const StreamReadInput& other) const {
        return path == other.path && chunk_size == other.chunk_size;
    }

    bool operator!=(const StreamReadInput& other) const {
        return !(*this == other);
    }
};

/**
 * @brief Result of a streaming write operation.
 */
struct StreamWriteResult {
    fs::path path;
    std::size_t bytes_written = 0;
    std::size_t chunks_written = 0;
    bool success = false;

    StreamWriteResult() = default;

    static StreamWriteResult success_result(fs::path p, std::size_t bytes,
                                            std::size_t chunks) {
        StreamWriteResult result;
        result.path = std::move(p);
        result.bytes_written = bytes;
        result.chunks_written = chunks;
        result.success = true;
        return result;
    }
};

}  // namespace dftracer::utils::utilities::io

// Hash specializations to enable caching
namespace std {
template <>
struct hash<dftracer::utils::utilities::io::StreamReadInput> {
    std::size_t operator()(
        const dftracer::utils::utilities::io::StreamReadInput& req)
        const noexcept {
        std::size_t h1 = std::hash<std::string>{}(req.path.string());
        std::size_t h2 = std::hash<std::size_t>{}(req.chunk_size);
        return h1 ^ (h2 << 1);
    }
};
}  // namespace std

#endif  // DFTRACER_UTILS_UTILITIES_IO_STREAMING_H
