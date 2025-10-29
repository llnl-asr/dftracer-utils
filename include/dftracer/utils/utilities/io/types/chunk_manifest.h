#ifndef DFTRACER_UTILS_UTILITIES_IO_TYPES_CHUNK_MANIFEST_H
#define DFTRACER_UTILS_UTILITIES_IO_TYPES_CHUNK_MANIFEST_H

#include <dftracer/utils/utilities/io/types/chunk_spec.h>

#include <vector>

namespace dftracer::utils::utilities::io {

/**
 * @brief Manifest of multiple chunk specifications.
 *
 * Represents a logical chunk manifest that describes multiple
 * source files or file ranges to be processed together.
 */
struct ChunkManifest {
    std::vector<ChunkSpec> specs;
    double total_size_mb;

    ChunkManifest() : total_size_mb(0.0) {}

    ChunkManifest(std::vector<ChunkSpec> chunk_specs, double total_mb)
        : specs(std::move(chunk_specs)), total_size_mb(total_mb) {}

    bool operator==(const ChunkManifest& other) const {
        return specs == other.specs && total_size_mb == other.total_size_mb;
    }

    bool operator!=(const ChunkManifest& other) const {
        return !(*this == other);
    }
};

}  // namespace dftracer::utils::utilities::io

// Hash specialization for caching
namespace std {
template <>
struct hash<dftracer::utils::utilities::io::ChunkManifest> {
    std::size_t operator()(const dftracer::utils::utilities::io::ChunkManifest&
                               manifest) const noexcept {
        std::size_t h1 = std::hash<double>{}(manifest.total_size_mb);
        std::size_t h2 = 0;
        for (const auto& spec : manifest.specs) {
            h2 ^= std::hash<dftracer::utils::utilities::io::ChunkSpec>{}(spec) +
                  0x9e3779b9 + (h2 << 6) + (h2 >> 2);
        }
        return h1 ^ (h2 << 1);
    }
};
}  // namespace std

#endif  // DFTRACER_UTILS_UTILITIES_IO_TYPES_CHUNK_MANIFEST_H
