#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/utilities/composites/dft/chunk_extractor_utility.h>
#include <dftracer/utils/utilities/composites/dft/event_hasher_utility.h>
#include <dftracer/utils/utilities/fileio/lines/streaming_line_reader.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

namespace dftracer::utils::utilities::composites::dft {

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
    std::string output_path = input.output_dir + "/" + input.app_name + "-" +
                              std::to_string(input.chunk_index) + ".pfw";

    ChunkExtractorUtilityOutput result;
    result.chunk_index = input.chunk_index;
    result.output_path = output_path;
    result.size_mb = 0.0;
    result.events = 0;
    result.success = false;

    // Open output file
    ssize_t open_result = co_await dftracer::utils::io::open(
        output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (open_result < 0) {
        DFTRACER_UTILS_LOG_ERROR("Cannot open output file: %s",
                                 output_path.c_str());
        co_return result;
    }
    int output_fd = static_cast<int>(open_result);

    // Write JSON array opening
    co_await dftracer::utils::io::write(output_fd, "[\n", 2);

    std::size_t total_events = 0;

    IncrementalEventHasher event_hasher;
    auto event_id_extractor = std::make_shared<EventIdExtractor>();

    // Process each chunk spec in the manifest
    for (const auto& spec : input.manifest.specs) {
        // Use line-based reading when line info is available for accurate
        // extraction
        if (spec.has_line_info()) {
            auto reader_config =
                StreamingLineReaderConfig()
                    .with_file(spec.file_path)
                    .with_index(spec.idx_path)
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
                    // Write valid JSON event
                    co_await dftracer::utils::io::write(output_fd, trimmed,
                                                        trimmed_length);
                    co_await dftracer::utils::io::write(output_fd, "\n", 1);

                    auto extract_input = EventIdExtractionInput::from_json(
                        std::string_view(trimmed, trimmed_length));
                    EventId event_id =
                        co_await event_id_extractor->process(extract_input);
                    if (event_id.is_valid()) {
                        event_hasher.update(event_id);
                    }

                    total_events++;
                }
            }
        } else {
            // Fallback to byte-based reading when line info not available
            if (!spec.idx_path.empty()) {
                // Compressed/indexed file - use byte-based reading with Reader
                auto reader = reader::internal::ReaderFactory::create(
                    spec.file_path, spec.idx_path);
                auto line_gen = sources::async_indexed_file_bytes(
                    reader, spec.start_byte, spec.end_byte);

                while (auto line_opt = co_await line_gen.next()) {
                    const auto& line = *line_opt;
                    // Validate and filter JSON events
                    const char* trimmed;
                    std::size_t trimmed_length;
                    if (json_trim_and_validate(line.content.data(),
                                               line.content.length(), trimmed,
                                               trimmed_length) &&
                        trimmed_length > 8) {
                        // Write valid JSON event
                        co_await dftracer::utils::io::write(output_fd, trimmed,
                                                            trimmed_length);
                        co_await dftracer::utils::io::write(output_fd, "\n", 1);

                        auto extract_input = EventIdExtractionInput::from_json(
                            std::string_view(trimmed, trimmed_length));
                        EventId event_id =
                            co_await event_id_extractor->process(extract_input);
                        if (event_id.is_valid()) {
                            event_hasher.update(event_id);
                        }

                        total_events++;
                    }
                }
            } else {
                // Plain text file - use byte-based reading
                auto line_gen = sources::async_plain_file_bytes(
                    spec.file_path, spec.start_byte, spec.end_byte);

                while (auto line_opt = co_await line_gen.next()) {
                    const auto& line = *line_opt;
                    // Validate and filter JSON events
                    const char* trimmed;
                    std::size_t trimmed_length;
                    if (json_trim_and_validate(line.content.data(),
                                               line.content.length(), trimmed,
                                               trimmed_length) &&
                        trimmed_length > 8) {
                        // Write valid JSON event
                        co_await dftracer::utils::io::write(output_fd, trimmed,
                                                            trimmed_length);
                        co_await dftracer::utils::io::write(output_fd, "\n", 1);

                        auto extract_input = EventIdExtractionInput::from_json(
                            std::string_view(trimmed, trimmed_length));
                        EventId event_id =
                            co_await event_id_extractor->process(extract_input);
                        if (event_id.is_valid()) {
                            event_hasher.update(event_id);
                        }

                        total_events++;
                    }
                }
            }
        }
    }

    // Write JSON array closing
    co_await dftracer::utils::io::write(output_fd, "]\n", 2);
    co_await dftracer::utils::io::close(output_fd);

    result.events = total_events;
    result.size_mb = input.manifest.total_size_mb;
    result.event_hash = event_hasher.get_hash();

    DFTRACER_UTILS_LOG_DEBUG("Chunk %d: Extracted %zu events, hash=0x%zx",
                             input.chunk_index, total_events,
                             result.event_hash);

    // Compress if requested
    if (input.compress && total_events > 0) {
        std::string compressed_path = output_path + ".gz";
        if (compress_output(output_path, compressed_path)) {
            if (fs::exists(compressed_path)) {
                fs::remove(output_path);
                result.output_path = compressed_path;
            }
        }
    }

    result.success = true;

    DFTRACER_UTILS_LOG_DEBUG("Chunk %d: %zu events, %.2f MB written to %s",
                             input.chunk_index, result.events, result.size_mb,
                             result.output_path.c_str());

    co_return result;
}

bool ChunkExtractorUtility::compress_output(const std::string& input_path,
                                            const std::string& output_path) {
    std::ifstream infile(input_path, std::ios::binary);
    std::ofstream outfile(output_path, std::ios::binary);

    if (!infile || !outfile) {
        DFTRACER_UTILS_LOG_ERROR("%s", "Cannot open files for compression");
        return false;
    }

    z_stream strm{};
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        DFTRACER_UTILS_LOG_ERROR("%s", "Failed to initialize zlib");
        return false;
    }

    constexpr std::size_t BUFFER_SIZE = 64 * 1024;
    std::vector<unsigned char> in_buffer(BUFFER_SIZE);
    std::vector<unsigned char> out_buffer(BUFFER_SIZE);

    int flush = Z_NO_FLUSH;
    do {
        infile.read(reinterpret_cast<char*>(in_buffer.data()), BUFFER_SIZE);
        std::streamsize bytes_read = infile.gcount();

        if (bytes_read == 0) break;

        strm.avail_in = static_cast<uInt>(bytes_read);
        strm.next_in = in_buffer.data();
        flush = infile.eof() ? Z_FINISH : Z_NO_FLUSH;

        do {
            strm.avail_out = BUFFER_SIZE;
            strm.next_out = out_buffer.data();
            deflate(&strm, flush);

            std::size_t bytes_to_write = BUFFER_SIZE - strm.avail_out;
            outfile.write(reinterpret_cast<const char*>(out_buffer.data()),
                          bytes_to_write);
        } while (strm.avail_out == 0);
    } while (flush != Z_FINISH);

    deflateEnd(&strm);
    infile.close();
    outfile.close();

    return true;
}

}  // namespace dftracer::utils::utilities::composites::dft
