#ifndef DFTRACER_UTILS_UTILITIES_IO_TYPES_CHUNK_SPEC_H
#define DFTRACER_UTILS_UTILITIES_IO_TYPES_CHUNK_SPEC_H

#include <cstddef>
#include <string>

namespace dftracer::utils::utilities::io {

/**
 * @brief Specification for a chunk to read from a file.
 *
 * Describes which file and byte range to read from.
 * Used for chunked file processing and splitting operations.
 */
struct ChunkSpec {
    std::string file_path;
    std::string idx_path;    // Empty for plain text files
    double size_mb;
    std::size_t start_byte;  // Starting byte offset (0-based)
    std::size_t end_byte;    // Ending byte offset (exclusive)

    ChunkSpec() : size_mb(0.0), start_byte(0), end_byte(0) {}

    ChunkSpec(std::string path, std::string idx, double mb, std::size_t start,
              std::size_t end)
        : file_path(std::move(path)),
          idx_path(std::move(idx)),
          size_mb(mb),
          start_byte(start),
          end_byte(end) {}

    bool operator==(const ChunkSpec& other) const {
        return file_path == other.file_path && idx_path == other.idx_path &&
               size_mb == other.size_mb && start_byte == other.start_byte &&
               end_byte == other.end_byte;
    }

    bool operator!=(const ChunkSpec& other) const { return !(*this == other); }

    std::size_t size_bytes() const {
        return (end_byte > start_byte) ? (end_byte - start_byte) : 0;
    }
};

}  // namespace dftracer::utils::utilities::io

// Hash specialization for caching
namespace std {
template <>
struct hash<dftracer::utils::utilities::io::ChunkSpec> {
    std::size_t operator()(
        const dftracer::utils::utilities::io::ChunkSpec& spec) const noexcept {
        std::size_t h1 = std::hash<std::string>{}(spec.file_path);
        std::size_t h2 = std::hash<std::string>{}(spec.idx_path);
        std::size_t h3 = std::hash<double>{}(spec.size_mb);
        std::size_t h4 = std::hash<std::size_t>{}(spec.start_byte);
        std::size_t h5 = std::hash<std::size_t>{}(spec.end_byte);
        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3) ^ (h5 << 4);
    }
};
}  // namespace std

#endif  // DFTRACER_UTILS_UTILITIES_IO_TYPES_CHUNK_SPEC_H
