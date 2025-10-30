#include <dftracer/utils/utilities/composites/dft/chunk_manifest_mapper_utility.h>

#include <algorithm>
#include <cmath>

namespace dftracer::utils::utilities::composites::dft {

ChunkManifestMapperUtilityOutput ChunkManifestMapperUtility::process(
    const ChunkManifestMapperUtilityInput& input) {
    std::vector<internal::DFTracerChunkManifest> manifests;

    internal::DFTracerChunkManifest current_manifest;
    current_manifest.total_size_mb = 0;

    for (const auto& file : input.file_metadata) {
        if (!file.success || file.size_mb <= 0 || file.valid_events == 0)
            continue;

        std::size_t remaining_events = file.valid_events;
        std::size_t current_start = file.start_line;
        std::size_t total_lines = file.end_line - file.start_line + 1;

        while (remaining_events > 0) {
            double available_space =
                input.target_chunk_size_mb - current_manifest.total_size_mb;

            std::size_t events_that_fit = 0;
            if (available_space > 0 && file.size_per_line > 0) {
                events_that_fit = static_cast<std::size_t>(
                    std::floor(available_space / file.size_per_line));
            }

            // Always respect chunk size limit
            std::size_t events_to_take =
                (events_that_fit > 0)
                    ? std::min(remaining_events, events_that_fit)
                    : remaining_events;

            if (events_to_take == 0 && remaining_events > 0) {
                if (!current_manifest.specs.empty()) {
                    manifests.push_back(current_manifest);
                    current_manifest = internal::DFTracerChunkManifest();
                    current_manifest.total_size_mb = 0;
                }
                continue;
            }

            double event_ratio = static_cast<double>(events_to_take) /
                                 static_cast<double>(file.valid_events);
            std::size_t lines_to_take = static_cast<std::size_t>(
                std::ceil(event_ratio * static_cast<double>(total_lines)));

            std::size_t available_lines = file.end_line - current_start + 1;
            if (lines_to_take > available_lines) {
                lines_to_take = available_lines;
            }

            double size_to_take =
                static_cast<double>(events_to_take) * file.size_per_line;

            // Create DFTracerChunkSpec with both byte offsets and line tracking
            // Approximate byte offsets assuming uniform distribution across
            // lines
            std::size_t line_end = current_start + lines_to_take - 1;
            if (line_end > file.end_line) {
                line_end = file.end_line;
            }

            // Calculate byte offsets
            double bytes_per_line = file.size_mb * 1024.0 * 1024.0 /
                                    static_cast<double>(total_lines);
            std::size_t start_byte = static_cast<std::size_t>(
                static_cast<double>(current_start - file.start_line) *
                bytes_per_line);
            std::size_t end_byte = static_cast<std::size_t>(
                static_cast<double>(line_end - file.start_line + 1) *
                bytes_per_line);

            internal::DFTracerChunkSpec spec(file.file_path, file.idx_path,
                                             size_to_take, start_byte, end_byte,
                                             current_start, line_end);

            current_manifest.add_spec(spec);

            current_start = line_end + 1;
            remaining_events -= events_to_take;

            // Finalize chunk if it's 95% full
            if (current_manifest.total_size_mb >=
                input.target_chunk_size_mb * 0.95) {
                manifests.push_back(current_manifest);
                current_manifest = internal::DFTracerChunkManifest();
                current_manifest.total_size_mb = 0;
            }
        }
    }

    // Add final manifest if not empty
    if (!current_manifest.specs.empty()) {
        manifests.push_back(current_manifest);
    }

    return manifests;
}

}  // namespace dftracer::utils::utilities::composites::dft
