#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/composites/indexed_file_reader_utility.h>
#include <dftracer/utils/utilities/fileio/lines/streaming_line_reader.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>

namespace dftracer::utils::utilities::composites::dft {

namespace hash = dftracer::utils::utilities::hash;
using namespace fileio::lines;

coro::CoroTask<MetadataCollectorUtilityOutput>
MetadataCollectorUtility::process(const MetadataCollectorUtilityInput& input) {
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
            co_return co_await process_compressed(modified_input);
        } else {
            // Plain text file
            co_return co_await process_plain(input);
        }
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Failed to collect metadata for %s: %s",
                                 input.file_path.c_str(), e.what());
        co_return meta;
    }
}

coro::CoroTask<MetadataCollectorUtilityOutput>
MetadataCollectorUtility::process_compressed(
    const MetadataCollectorUtilityInput& input) {
    MetadataCollectorUtilityOutput meta;
    meta.file_path = input.file_path;
    meta.idx_path = input.idx_path;

    try {
        // Detect format
        meta.format = dftracer::utils::utilities::indexer::internal::
            IndexerFactory::detect_format(input.file_path);
        meta.compressed_size = fs::file_size(input.file_path);

        // Check if index exists
        meta.has_index = fs::exists(input.idx_path);

        // Create or load indexer
        std::shared_ptr<dftracer::utils::utilities::indexer::internal::Indexer>
            indexer;
        if (!meta.has_index || input.force_rebuild) {
            if (input.force_rebuild && meta.has_index) {
                DFTRACER_UTILS_LOG_DEBUG("Removing existing index: %s",
                                         input.idx_path.c_str());
                fs::remove(input.idx_path);
            }
            DFTRACER_UTILS_LOG_DEBUG("Building index for: %s",
                                     input.file_path.c_str());
            indexer = dftracer::utils::utilities::indexer::internal::
                IndexerFactory::create(input.file_path, input.idx_path,
                                       input.checkpoint_size, true);
            co_await indexer->build_async();
            meta.has_index = true;
        } else {
            indexer = dftracer::utils::utilities::indexer::internal::
                IndexerFactory::create(input.file_path, input.idx_path,
                                       input.checkpoint_size, false);
            if (indexer->need_rebuild()) {
                DFTRACER_UTILS_LOG_DEBUG("Index needs rebuild: %s",
                                         input.idx_path.c_str());
                meta.index_valid = false;
                fs::remove(input.idx_path);
                indexer = dftracer::utils::utilities::indexer::internal::
                    IndexerFactory::create(input.file_path, input.idx_path,
                                           input.checkpoint_size, true);
                co_await indexer->build_async();
            }
        }

        meta.index_valid = true;

        // Get metadata from indexer
        std::size_t total_lines = indexer->get_num_lines();
        meta.num_lines = total_lines;
        meta.uncompressed_size = indexer->get_max_bytes();
        meta.checkpoint_size = indexer->get_checkpoint_size();
        meta.num_checkpoints = indexer->get_checkpoints().size();

        if (total_lines == 0) {
            DFTRACER_UTILS_LOG_DEBUG("File %s has no lines",
                                     input.file_path.c_str());
            meta.success = true;
            co_return meta;
        }

        std::size_t file_size_bytes = fs::file_size(input.file_path);
        double size_mb =
            static_cast<double>(file_size_bytes) / (1024.0 * 1024.0);

        if (input.count_lines) {
            // Full read: compute content hash and accurate event count
            std::size_t content_hash = 0;
            std::size_t actual_valid_events = 0;
            hash::HasherUtility hasher;
            {
                auto line_gen = StreamingLineReader::read_async(
                    StreamingLineReaderConfig()
                        .with_file(input.file_path)
                        .with_index(input.idx_path)
                        .with_line_range(1, total_lines));
                while (auto line_opt = co_await line_gen.next()) {
                    const auto& line = *line_opt;
                    const char* trimmed;
                    std::size_t trimmed_length;
                    if (json_trim_and_validate(line.content.data(),
                                               line.content.length(), trimmed,
                                               trimmed_length) &&
                        trimmed_length > 8) {
                        hasher.reset();
                        hasher.update(
                            std::string_view(trimmed, trimmed_length));
                        content_hash += hasher.get_hash().value;
                        actual_valid_events++;
                    }
                }
            }
            meta.valid_events = actual_valid_events;
            meta.event_hash = content_hash;
        } else {
            // Fast path: estimate from indexer line count
            std::size_t estimated_valid_events =
                (total_lines > 2) ? (total_lines - 2) : 0;
            meta.valid_events = estimated_valid_events;
            meta.event_hash = 0;
        }

        meta.size_mb = size_mb;
        meta.start_line = 1;
        meta.end_line = total_lines;
        meta.size_per_line =
            (meta.valid_events > 0)
                ? size_mb / static_cast<double>(meta.valid_events)
                : 0;
        meta.success = true;

        DFTRACER_UTILS_LOG_DEBUG(
            "File %s: %.2f MB, %zu valid events from %zu lines, %.8f MB/event",
            input.file_path.c_str(), meta.size_mb, meta.valid_events,
            total_lines, meta.size_per_line);

    } catch (const std::exception& e) {
        meta.error_message = e.what();
        meta.success = false;
    }

    co_return meta;
}

coro::CoroTask<MetadataCollectorUtilityOutput>
MetadataCollectorUtility::process_plain(
    const MetadataCollectorUtilityInput& input) {
    MetadataCollectorUtilityOutput meta;
    meta.file_path = input.file_path;
    meta.idx_path = "";

    try {
        // Plain file metadata
        meta.format = ArchiveFormat::UNKNOWN;  // Plain text file
        meta.has_index = false;
        meta.index_valid = false;
        std::size_t file_size = fs::file_size(input.file_path);
        meta.uncompressed_size = file_size;
        meta.compressed_size = file_size;  // No compression for plain files

        if (input.count_lines) {
            // Full read: count lines, events, compute hash
            auto line_gen =
                StreamingLineReader::read_plain_async(input.file_path);

            std::size_t total_lines = 0;
            std::size_t total_bytes = 0;
            std::size_t valid_events = 0;
            std::size_t content_hash = 0;
            hash::HasherUtility hasher;

            while (auto line_opt = co_await line_gen.next()) {
                const auto& line = *line_opt;
                total_lines++;
                const char* trimmed;
                std::size_t trimmed_length;
                if (json_trim_and_validate(line.content.data(),
                                           line.content.length(), trimmed,
                                           trimmed_length) &&
                    trimmed_length > 8) {
                    total_bytes += line.content.length();
                    valid_events++;
                    hasher.reset();
                    hasher.update(std::string_view(trimmed, trimmed_length));
                    content_hash += hasher.get_hash().value;
                }
            }

            meta.num_lines = total_lines;
            meta.size_mb = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
            meta.start_line = 1;
            meta.end_line = total_lines;
            meta.valid_events = valid_events;
            meta.event_hash = content_hash;
            meta.size_per_line =
                (valid_events > 0)
                    ? meta.size_mb / static_cast<double>(valid_events)
                    : 0;
        } else {
            // Fast path: estimate from file size, no line reading
            double size_mb = static_cast<double>(file_size) / (1024.0 * 1024.0);
            // Estimate ~200 bytes per event line for DFTracer JSON
            constexpr double ESTIMATED_BYTES_PER_EVENT = 200.0;
            std::size_t estimated_events = static_cast<std::size_t>(
                static_cast<double>(file_size) / ESTIMATED_BYTES_PER_EVENT);
            if (estimated_events < 1 && file_size > 0) {
                estimated_events = 1;
            }
            meta.num_lines = 0;
            meta.size_mb = size_mb;
            meta.start_line = 0;
            meta.end_line = 0;
            meta.valid_events = estimated_events;
            meta.event_hash = 0;
            meta.size_per_line =
                (estimated_events > 0)
                    ? size_mb / static_cast<double>(estimated_events)
                    : 0;
        }
        meta.success = true;

        DFTRACER_UTILS_LOG_DEBUG(
            "File %s: %.2f MB, %zu valid events from %zu lines, %.8f MB/event",
            input.file_path.c_str(), meta.size_mb, meta.valid_events,
            meta.num_lines, meta.size_per_line);

    } catch (const std::exception& e) {
        meta.error_message = e.what();
        meta.success = false;
    }

    co_return meta;
}

}  // namespace dftracer::utils::utilities::composites::dft
