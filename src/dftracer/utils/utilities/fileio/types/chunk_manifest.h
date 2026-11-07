// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_TYPES_CHUNK_MANIFEST_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_TYPES_CHUNK_MANIFEST_H

#include <dftracer/utils/dstd/hasher.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/utilities/fileio/types/chunk_spec.h>

namespace dftracer::utils::utilities::fileio {

/**
 * @brief Manifest of multiple chunk specifications.
 *
 * Represents a logical chunk manifest that describes multiple
 * source files or file ranges to be processed together.
 */
struct ChunkManifest {
    dstd::Vector<ChunkSpec> specs;
    double total_size_mb;

    ChunkManifest() : total_size_mb(0.0) {}

    ChunkManifest(dstd::Vector<ChunkSpec> chunk_specs, double total_mb)
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
namespace dftracer::utils::dstd {
template <>
struct Hash<dftracer::utils::utilities::fileio::ChunkManifest> {
    using is_avalanching = void;

    std::size_t operator()(
        const dftracer::utils::utilities::fileio::ChunkManifest& manifest)
        const noexcept {
        ::dftracer::utils::utilities::hash::HasherUtility hasher;
        hasher.update(manifest.total_size_mb);
        for (const auto& spec : manifest.specs) {
            hasher.update(
                Hash<::dftracer::utils::utilities::fileio::ChunkSpec>{}(spec));
        }
        return static_cast<std::size_t>(hash::splitmix64(hasher.hash().value));
    }
};
}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_TYPES_CHUNK_MANIFEST_H
