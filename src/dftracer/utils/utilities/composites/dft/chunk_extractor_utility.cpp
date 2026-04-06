#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/utilities/composites/dft/chunk_extractor_utility.h>
#include <dftracer/utils/utilities/fileio/chunk_writer.h>
#include <dftracer/utils/utilities/fileio/lines/streaming_line_reader.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>

namespace dftracer::utils::utilities::composites::dft {

namespace hash = dftracer::utils::utilities::hash;

using fileio::ChunkWriter;
using fileio::ChunkWriterConfig;
using namespace fileio::lines;

coro::CoroTask<ChunkExtractorUtilityOutput> ChunkExtractorUtility::process(
    const ChunkExtractorUtilityInput& input) {
    ChunkExtractorUtilityOutput result;
    result.chunk_index = input.chunk_index;
    result.success = false;

    try {
        co_return co_await extract_and_write(input);
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Failed to extract chunk %d: %s",
                                 input.chunk_index, e.what());
        result.output_path = input.output_dir + "/" + input.app_name + "-" +
                             std::to_string(input.chunk_index) + ".pfw";
        co_return result;
    }
}

coro::CoroTask<ChunkExtractorUtilityOutput>
ChunkExtractorUtility::extract_and_write(
    const ChunkExtractorUtilityInput& input) {
    ChunkExtractorUtilityOutput result;
    result.chunk_index = input.chunk_index;
    result.size_mb = 0.0;
    result.events = 0;
    result.success = false;

    ChunkWriterConfig writer_config;
    writer_config.output_dir = input.output_dir;
    writer_config.base_name =
        input.app_name + "-" + std::to_string(input.chunk_index);
    writer_config.chunk_size_bytes = std::numeric_limits<std::size_t>::max();
    writer_config.compress = input.compress;

    ChunkWriter writer(writer_config);
    co_await writer.open();

    std::size_t content_hash = 0;
    hash::HasherUtility hasher;

    for (const auto& spec : input.manifest.specs) {
        if (spec.has_line_info()) {
            auto reader_config =
                StreamingLineReaderConfig()
                    .with_file(spec.file_path)
                    .with_index(spec.index_path)
                    .with_line_range(spec.start_line, spec.end_line);
            auto line_gen = StreamingLineReader::read_async(reader_config);

            while (auto line_opt = co_await line_gen.next()) {
                const auto& line = *line_opt;
                const char* trimmed;
                std::size_t trimmed_length;
                if (json_trim_and_validate(line.content.data(),
                                           line.content.length(), trimmed,
                                           trimmed_length) &&
                    trimmed_length > 8) {
                    co_await writer.write_line(
                        ByteView(trimmed, trimmed_length));

                    if (input.compute_hash) {
                        hasher.reset();
                        hasher.update(
                            std::string_view(trimmed, trimmed_length));
                        content_hash += hasher.get_hash().value;
                    }
                }
            }
        } else {
            if (!spec.index_path.empty()) {
                auto reader = reader::internal::ReaderFactory::create(
                    spec.file_path, spec.index_path);
                auto line_gen = sources::async_indexed_file_bytes(
                    reader, spec.start_byte, spec.end_byte);

                while (auto line_opt = co_await line_gen.next()) {
                    const auto& line = *line_opt;
                    const char* trimmed;
                    std::size_t trimmed_length;
                    if (json_trim_and_validate(line.content.data(),
                                               line.content.length(), trimmed,
                                               trimmed_length) &&
                        trimmed_length > 8) {
                        co_await writer.write_line(
                            ByteView(trimmed, trimmed_length));

                        if (input.compute_hash) {
                            hasher.reset();
                            hasher.update(
                                std::string_view(trimmed, trimmed_length));
                            content_hash += hasher.get_hash().value;
                        }
                    }
                }
            } else {
                auto line_gen = sources::async_plain_file_bytes(
                    spec.file_path, spec.start_byte, spec.end_byte);

                while (auto line_opt = co_await line_gen.next()) {
                    const auto& line = *line_opt;
                    const char* trimmed;
                    std::size_t trimmed_length;
                    if (json_trim_and_validate(line.content.data(),
                                               line.content.length(), trimmed,
                                               trimmed_length) &&
                        trimmed_length > 8) {
                        co_await writer.write_line(
                            ByteView(trimmed, trimmed_length));

                        if (input.compute_hash) {
                            hasher.reset();
                            hasher.update(
                                std::string_view(trimmed, trimmed_length));
                            content_hash += hasher.get_hash().value;
                        }
                    }
                }
            }
        }
    }

    co_await writer.close();

    result.output_path = writer.chunks().empty() ? "" : writer.chunks()[0].path;
    result.events = writer.total_events_written();
    result.size_mb = input.manifest.total_size_mb;
    result.event_hash = content_hash;
    result.success = true;

    DFTRACER_UTILS_LOG_DEBUG(
        "Chunk %d: %zu events, %.2f MB written to %s (hash=0x%zx)",
        input.chunk_index, result.events, result.size_mb,
        result.output_path.c_str(), result.event_hash);

    co_return result;
}

}  // namespace dftracer::utils::utilities::composites::dft
