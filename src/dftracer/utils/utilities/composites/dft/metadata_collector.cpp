#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/indexer/indexer_factory.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector.h>
#include <dftracer/utils/utilities/composites/indexed_file_reader.h>
#include <dftracer/utils/utilities/io/lines/streaming_line_reader.h>

#include <atomic>

namespace dftracer::utils::utilities::composites::dft {

using namespace io::lines;

MetadataCollectorUtilityOutput MetadataCollectorUtility::process(
    const MetadataCollectorUtilityInput& input) {
    MetadataCollectorUtilityOutput meta;
    meta.file_path = input.file_path;
    meta.success = false;

    try {
        // Auto-detect file format based on extension
        std::string file_path = input.file_path;
        bool is_compressed = (file_path.size() > 3 &&
                              file_path.substr(file_path.size() - 3) == ".gz");

        if (is_compressed) {
            // Compressed file - generate index path if not provided
            MetadataCollectorUtilityInput modified_input = input;
            if (modified_input.idx_path.empty()) {
                // Auto-generate index path
                modified_input.idx_path = file_path + ".idx";
            }
            meta.idx_path = modified_input.idx_path;
            return process_compressed(modified_input);
        } else {
            // Plain text file
            return process_plain(input);
        }
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Failed to collect metadata for %s: %s",
                                 input.file_path.c_str(), e.what());
        return meta;
    }
}

MetadataCollectorUtilityOutput MetadataCollectorUtility::process_compressed(
    const MetadataCollectorUtilityInput& input) {
    MetadataCollectorUtilityOutput meta;
    meta.file_path = input.file_path;
    meta.idx_path = input.idx_path;

    auto indexer =
        IndexerFactory::create(input.file_path, input.idx_path,
                               input.checkpoint_size, input.force_rebuild);

    std::size_t total_lines = indexer->get_num_lines();
    if (total_lines == 0) {
        DFTRACER_UTILS_LOG_DEBUG("File %s has no lines",
                                 input.file_path.c_str());
        return meta;
    }

    std::size_t file_size_bytes = fs::file_size(input.file_path);
    double size_mb = static_cast<double>(file_size_bytes) / (1024.0 * 1024.0);

    // Estimate: First line is '[', last line is ']', rest are events
    // This is a good approximation for Chrome trace format
    std::size_t estimated_valid_events =
        (total_lines > 2) ? (total_lines - 2) : 0;

    DFTRACER_UTILS_LOG_DEBUG(
        "Estimated %zu valid events from %zu lines for %s (no file read)",
        estimated_valid_events, total_lines, input.file_path.c_str());

    meta.size_mb = size_mb;
    meta.start_line = 1;
    meta.end_line = total_lines;
    meta.valid_events = estimated_valid_events;
    meta.size_per_line =
        (estimated_valid_events > 0)
            ? size_mb / static_cast<double>(estimated_valid_events)
            : 0;
    meta.success = true;

    DFTRACER_UTILS_LOG_DEBUG(
        "File %s: %.2f MB, %zu valid events from %zu lines, %.8f MB/event",
        input.file_path.c_str(), meta.size_mb, meta.valid_events, total_lines,
        meta.size_per_line);

    return meta;
}

MetadataCollectorUtilityOutput MetadataCollectorUtility::process_plain(
    const MetadataCollectorUtilityInput& input) {
    MetadataCollectorUtilityOutput meta;
    meta.file_path = input.file_path;
    meta.idx_path = "";

    // For plain files, we need to read sequentially since we can't seek
    // But we can still validate JSON in parallel by batching lines
    auto line_range = StreamingLineReader::read_plain(input.file_path);

    std::size_t total_lines = 0;
    std::size_t total_bytes = 0;
    std::size_t valid_events = 0;

    while (line_range.has_next()) {
        Line line = line_range.next();
        total_lines++;
        const char* trimmed;
        std::size_t trimmed_length;
        if (json_trim_and_validate(line.content.data(), line.content.length(),
                                   trimmed, trimmed_length) &&
            trimmed_length > 8) {
            total_bytes += line.content.length();
            valid_events++;
        }
    }

    meta.size_mb = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
    meta.start_line = 1;
    meta.end_line = total_lines;
    meta.valid_events = valid_events;
    meta.size_per_line = (valid_events > 0)
                             ? meta.size_mb / static_cast<double>(valid_events)
                             : 0;
    meta.success = true;

    DFTRACER_UTILS_LOG_DEBUG(
        "File %s: %.2f MB, %zu valid events from %zu lines, %.8f MB/event",
        input.file_path.c_str(), meta.size_mb, valid_events, total_lines,
        meta.size_per_line);

    return meta;
}

}  // namespace dftracer::utils::utilities::composites::dft
