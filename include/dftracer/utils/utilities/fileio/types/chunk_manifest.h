#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_TYPES_CHUNK_MANIFEST_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_TYPES_CHUNK_MANIFEST_H

#include <dftracer/utils/utilities/fileio/types/chunk_spec.h>

#include <vector>

namespace dftracer::utils::utilities::fileio {

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

}  // namespace dftracer::utils::utilities::fileio

// Hash specialization for caching
namespace std {
template <>
struct hash<dftracer::utils::utilities::fileio::ChunkManifest> {
    std::size_t operator()(
        const dftracer::utils::utilities::fileio::ChunkManifest& manifest)
        const noexcept {
        ::dftracer::utils::utilities::hash::HasherUtility hasher;
        hasher.update(manifest.total_size_mb);
        for (const auto& spec : manifest.specs) {
            hasher.update(
                std::hash<::dftracer::utils::utilities::fileio::ChunkSpec>{}(
                    spec));
        }
        return hasher.get_hash().value;
    }
};
}  // namespace std

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_TYPES_CHUNK_MANIFEST_H
