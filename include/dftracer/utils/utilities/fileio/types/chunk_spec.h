#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_TYPES_CHUNK_SPEC_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_TYPES_CHUNK_SPEC_H

#include <dftracer/utils/utilities/hash/hasher_utility.h>

#include <cstddef>
#include <string>

namespace dftracer::utils::utilities::fileio {

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

}  // namespace dftracer::utils::utilities::fileio

// Hash specialization for caching
namespace std {
template <>
struct hash<dftracer::utils::utilities::fileio::ChunkSpec> {
    std::size_t operator()(const dftracer::utils::utilities::fileio::ChunkSpec&
                               spec) const noexcept {
        ::dftracer::utils::utilities::hash::HasherUtility hasher;
        hasher.update(spec.file_path);
        hasher.update(spec.idx_path);
        hasher.update(spec.size_mb);
        hasher.update(spec.start_byte);
        hasher.update(spec.end_byte);
        return hasher.get_hash().value;
    }
};
}  // namespace std

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_TYPES_CHUNK_SPEC_H
