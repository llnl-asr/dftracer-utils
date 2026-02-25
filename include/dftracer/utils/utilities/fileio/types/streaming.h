#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_STREAMING_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_STREAMING_H

#include <dftracer/utils/utilities/fileio/types/types.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>

#include <functional>
#include <vector>

namespace dftracer::utils::utilities::fileio {

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

}  // namespace dftracer::utils::utilities::fileio

// Hash specializations to enable caching
namespace std {
template <>
struct hash<dftracer::utils::utilities::fileio::StreamReadInput> {
    std::size_t operator()(
        const dftracer::utils::utilities::fileio::StreamReadInput& req)
        const noexcept {
        ::dftracer::utils::utilities::hash::HasherUtility hasher;
        hasher.update(req.path.string());
        hasher.update(req.chunk_size);
        return hasher.get_hash().value;
    }
};
}  // namespace std

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_STREAMING_H
