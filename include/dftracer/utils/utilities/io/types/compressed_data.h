#ifndef DFTRACER_UTILS_UTILITIES_IO_TYPES_COMPRESSED_DATA_H
#define DFTRACER_UTILS_UTILITIES_IO_TYPES_COMPRESSED_DATA_H

#include <cstddef>
#include <vector>

namespace dftracer::utils::utilities::io {

/**
 * @brief Compressed binary data structure.
 *
 * This represents compressed data along with metadata about the compression.
 * Used by:
 * - Compression utilities (gzip, bzip2, etc.)
 * - File writers that support compression
 */
struct CompressedData {
    std::vector<unsigned char> data;
    std::size_t original_size = 0;  // Original uncompressed size

    CompressedData() = default;

    explicit CompressedData(std::vector<unsigned char> d,
                            std::size_t orig_size = 0)
        : data(std::move(d)), original_size(orig_size) {}

    // Equality for caching support
    bool operator==(const CompressedData& other) const {
        return data == other.data && original_size == other.original_size;
    }

    bool operator!=(const CompressedData& other) const {
        return !(*this == other);
    }

    // Get compression ratio
    double compression_ratio() const {
        if (original_size == 0) return 0.0;
        return static_cast<double>(data.size()) /
               static_cast<double>(original_size);
    }

    // Get space savings percentage
    double space_savings() const {
        if (original_size == 0) return 0.0;
        return (1.0 - compression_ratio()) * 100.0;
    }

    std::size_t size() const { return data.size(); }

    bool empty() const { return data.empty(); }
};

}  // namespace dftracer::utils::utilities::io

// Hash specialization for caching
namespace std {
template <>
struct hash<dftracer::utils::utilities::io::CompressedData> {
    std::size_t operator()(const dftracer::utils::utilities::io::CompressedData&
                               compressed) const noexcept {
        // Hash the data bytes
        std::size_t h = 0;
        for (const auto& byte : compressed.data) {
            h ^= std::hash<unsigned char>{}(byte) + 0x9e3779b9 + (h << 6) +
                 (h >> 2);
        }
        // Combine with original_size
        h ^= std::hash<std::size_t>{}(compressed.original_size) + 0x9e3779b9 +
             (h << 6) + (h >> 2);
        return h;
    }
};
}  // namespace std

#endif  // DFTRACER_UTILS_UTILITIES_IO_TYPES_COMPRESSED_DATA_H
