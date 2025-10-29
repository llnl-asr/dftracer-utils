#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFTRACER_CHUNK_MANIFEST_MAPPER_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFTRACER_CHUNK_MANIFEST_MAPPER_H

#include <dftracer/utils/core/utilities/utilities.h>
#include <dftracer/utils/utilities/composites/dft/chunk_manifest.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector.h>
#include <dftracer/utils/utilities/io/types/types.h>

#include <cstddef>
#include <vector>

namespace dftracer::utils::utilities::composites::dft {

/**
 * @brief Input for DFTracer chunk manifest mapping.
 */
struct ChunkManifestMapperUtilityInput {
    std::vector<MetadataCollectorUtilityOutput> file_metadata;
    double target_chunk_size_mb;

    ChunkManifestMapperUtilityInput() : target_chunk_size_mb(0) {}

    static ChunkManifestMapperUtilityInput from_metadata(
        std::vector<MetadataCollectorUtilityOutput> metadata) {
        ChunkManifestMapperUtilityInput input;
        input.file_metadata = std::move(metadata);
        return input;
    }

    ChunkManifestMapperUtilityInput& with_target_size(double size_mb) {
        target_chunk_size_mb = size_mb;
        return *this;
    }
};

/**
 * @brief Output: vector of DFTracer chunk manifests with line tracking.
 */
using ChunkManifestMapperUtilityOutput = std::vector<DFTracerChunkManifest>;

/**
 * @brief Workflow for mapping DFTracer file metadata to chunk manifests with
 * line tracking.
 *
 * This workflow takes DFTracer-specific file metadata (line counts, sizes,
 * event counts) and distributes files across chunks to achieve a target chunk
 * size. It creates DFTracerChunkManifest objects with both byte offsets and
 * line numbers.
 *
 * Algorithm:
 * - Greedily fills chunks up to target size
 * - Splits large files across multiple chunks if needed
 * - Tracks both byte offsets and line ranges for each chunk
 * - Approximates byte offsets from line-based metadata
 * - Assumes uniform byte distribution across lines
 * - Line boundary alignment happens during extraction (LineBytesRange)
 *
 * Usage:
 * @code
 * DFTracerChunkManifestMapper mapper;
 *
 * auto input = DFTracerChunkManifestMapperInput::from_metadata(file_metadata)
 *                  .with_target_size(100.0);  // 100 MB chunks
 *
 * auto manifests = mapper.process(input);
 * // manifests[i] contains DFTracerChunkSpec objects with line tracking
 * @endcode
 */
class ChunkManifestMapperUtility
    : public utilities::Utility<ChunkManifestMapperUtilityInput,
                                ChunkManifestMapperUtilityOutput> {
   public:
    ChunkManifestMapperUtilityOutput process(
        const ChunkManifestMapperUtilityInput& input) override;
};

}  // namespace dftracer::utils::utilities::composites::dft

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFTRACER_CHUNK_MANIFEST_MAPPER_H
