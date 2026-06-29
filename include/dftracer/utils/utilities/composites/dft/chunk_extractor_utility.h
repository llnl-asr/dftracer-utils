#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFTRACER_CHUNK_EXTRACTOR_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFTRACER_CHUNK_EXTRACTOR_UTILITY_H

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utilities/utilities.h>
#include <dftracer/utils/utilities/composites/dft/event_id_extractor_utility.h>
#include <dftracer/utils/utilities/composites/dft/internal/chunk_manifest.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_compressor_utility.h>
#include <dftracer/utils/utilities/fileio/types/types.h>

#include <cstddef>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::composites::dft {

/**
 * @brief Input for DFTracer chunk extraction.
 *
 * Accepts DFTracerChunkManifest with line tracking, but converts to
 * byte-based fileio::ChunkManifest for extraction.
 */
struct ChunkExtractorUtilityInput {
    int chunk_index;  // Application-level: which output chunk number
    internal::DFTracerChunkManifest manifest;
    std::string output_dir;
    std::string app_name;
    bool compress = false;
    bool compute_hash = true;

    ChunkExtractorUtilityInput()
        : chunk_index(0), compress(false), compute_hash(true) {}

    static ChunkExtractorUtilityInput from_manifest(
        int index, internal::DFTracerChunkManifest m) {
        ChunkExtractorUtilityInput input;
        input.chunk_index = index;
        input.manifest = std::move(m);
        return input;
    }

    ChunkExtractorUtilityInput& with_output_dir(std::string dir) {
        output_dir = std::move(dir);
        return *this;
    }

    ChunkExtractorUtilityInput& with_app_name(std::string name) {
        app_name = std::move(name);
        return *this;
    }

    ChunkExtractorUtilityInput& with_compression(bool enabled) {
        compress = enabled;
        return *this;
    }

    ChunkExtractorUtilityInput& with_compute_hash(bool enabled) {
        compute_hash = enabled;
        return *this;
    }

    // Convert to byte-based fileio::ChunkManifest for extraction
    fileio::ChunkManifest to_io_manifest() const {
        fileio::ChunkManifest io_manifest;
        io_manifest.total_size_mb = manifest.total_size_mb;
        for (const auto& dft_spec : manifest.specs) {
            fileio::ChunkSpec io_spec;
            io_spec.file_path = dft_spec.file_path;
            io_spec.index_path = dft_spec.index_path;
            io_spec.size_mb = dft_spec.size_mb;
            io_spec.start_byte = dft_spec.start_byte;
            io_spec.end_byte = dft_spec.end_byte;
            io_manifest.specs.push_back(io_spec);
        }
        return io_manifest;
    }

    bool operator==(const ChunkExtractorUtilityInput& other) const {
        return chunk_index == other.chunk_index && manifest == other.manifest &&
               output_dir == other.output_dir && app_name == other.app_name &&
               compress == other.compress && compute_hash == other.compute_hash;
    }
};

/**
 * @brief Result of DFTracer chunk extraction.
 */
struct ChunkExtractorUtilityOutput {
    int chunk_index;
    std::string output_path;
    double size_mb;
    std::size_t events;  // DFTracer-specific: number of JSON events
    bool success;

    std::size_t event_hash = 0;

    ChunkExtractorUtilityOutput()
        : chunk_index(0), size_mb(0.0), events(0), success(false) {}

    ChunkExtractorUtilityOutput(int index, std::string path, double mb,
                                std::size_t event_count, bool succ)
        : chunk_index(index),
          output_path(std::move(path)),
          size_mb(mb),
          events(event_count),
          success(succ) {}

    bool operator==(const ChunkExtractorUtilityOutput& other) const {
        return chunk_index == other.chunk_index &&
               output_path == other.output_path && size_mb == other.size_mb &&
               events == other.events && success == other.success;
    }

    bool operator!=(const ChunkExtractorUtilityOutput& other) const {
        return !(*this == other);
    }
};

/**
 * @brief Extracts and merges chunks from DFTracer files.
 *
 * Usage:
 * @code
 * ChunkExtractorUtility extractor;
 *
 * auto input = ChunkExtractorUtilityInput::from_manifest(1, manifest)
 *                  .with_output_dir("/output")
 *                  .with_app_name("myapp")
 *                  .with_compression(true);
 *
 * auto result = extractor.process(input);
 * if (result.success) {
 *     std::cout << "Extracted " << result.events << " events\n";
 * }
 * @endcode
 */
class ChunkExtractorUtility
    : public utilities::Utility<ChunkExtractorUtilityInput,
                                ChunkExtractorUtilityOutput> {
   public:
    coro::CoroTask<ChunkExtractorUtilityOutput> process(
        const ChunkExtractorUtilityInput& input) override;

   private:
    coro::CoroTask<ChunkExtractorUtilityOutput> extract_and_write(
        const ChunkExtractorUtilityInput& input);
};

}  // namespace dftracer::utils::utilities::composites::dft

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFTRACER_CHUNK_EXTRACTOR_UTILITY_H
