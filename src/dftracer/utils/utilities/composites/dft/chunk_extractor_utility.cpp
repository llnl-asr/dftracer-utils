#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/utilities/composites/dft/chunk_extractor_utility.h>
#include <dftracer/utils/utilities/io/lines/line_range.h>
#include <dftracer/utils/utilities/io/lines/streaming_line_reader.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>
#include <zlib.h>

#include <cstdio>
#include <fstream>

namespace dftracer::utils::utilities::composites::dft {

using namespace io::lines;

ChunkExtractorUtilityOutput ChunkExtractorUtility::process(
    const ChunkExtractorUtilityInput& input) {
    ChunkExtractorUtilityOutput result;
    result.chunk_index = input.chunk_index;
    result.success = false;

    try {
        return extract_and_write(input);
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Failed to extract chunk %d: %s",
                                 input.chunk_index, e.what());
        result.output_path = input.output_dir + "/" + input.app_name + "-" +
                             std::to_string(input.chunk_index) + ".pfw";
        return result;
    }
}

ChunkExtractorUtilityOutput ChunkExtractorUtility::extract_and_write(
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
    FILE* output_fp = std::fopen(output_path.c_str(), "w");
    if (!output_fp) {
        DFTRACER_UTILS_LOG_ERROR("Cannot open output file: %s",
                                 output_path.c_str());
        return result;
    }

    // Set larger buffer for better performance
    setvbuf(output_fp, nullptr, _IOFBF, 1024 * 1024);

    // Write JSON array opening
    std::fprintf(output_fp, "[\n");

    std::size_t total_events = 0;

    // NEW: Pre-allocate event_ids vector for performance
    result.event_ids.reserve(5000);

    // NEW: Create event ID extractor utility
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
            LineRange line_range = StreamingLineReader::read(reader_config);

            std::size_t line_count = 0;
            for (const auto& line : line_range) {
                line_count++;

                const char* trimmed;
                std::size_t trimmed_length;
                if (json_trim_and_validate(line.content.data(),
                                           line.content.length(), trimmed,
                                           trimmed_length) &&
                    trimmed_length > 8) {
                    // Write valid JSON event
                    std::fwrite(trimmed, 1, trimmed_length, output_fp);
                    std::fwrite("\n", 1, 1, output_fp);

                    // NEW: Extract and collect event ID for verification
                    auto extract_input = EventIdExtractionInput::from_json(
                        std::string_view(trimmed, trimmed_length));
                    EventId event_id =
                        event_id_extractor->process(extract_input);
                    if (event_id.is_valid()) {
                        result.event_ids.push_back(event_id);
                    }

                    total_events++;
                }
            }
        } else {
            // Fallback to byte-based reading when line info not available
            LineBytesRange line_range;

            if (!spec.idx_path.empty()) {
                // Compressed/indexed file - use byte-based reading with Reader
                auto reader = reader::internal::ReaderFactory::create(
                    spec.file_path, spec.idx_path);
                line_range = LineBytesRange::from_indexed_file(
                    reader, spec.start_byte, spec.end_byte);
            } else {
                // Plain text file - use byte-based reading
                line_range = LineBytesRange::from_plain_file(
                    spec.file_path, spec.start_byte, spec.end_byte);
            }

            // Process all lines using C++ iterator API (range-based for loop)
            for (const auto& line : line_range) {
                // Validate and filter JSON events
                const char* trimmed;
                std::size_t trimmed_length;
                if (json_trim_and_validate(line.content.data(),
                                           line.content.length(), trimmed,
                                           trimmed_length) &&
                    trimmed_length > 8) {
                    // Write valid JSON event
                    std::fwrite(trimmed, 1, trimmed_length, output_fp);
                    std::fwrite("\n", 1, 1, output_fp);

                    // NEW: Extract and collect event ID for verification
                    auto extract_input = EventIdExtractionInput::from_json(
                        std::string_view(trimmed, trimmed_length));
                    EventId event_id =
                        event_id_extractor->process(extract_input);
                    if (event_id.is_valid()) {
                        result.event_ids.push_back(event_id);
                    }

                    total_events++;
                }
            }
        }
    }

    // Write JSON array closing
    std::fprintf(output_fp, "]\n");
    std::fclose(output_fp);

    result.events = total_events;
    result.size_mb = input.manifest.total_size_mb;

    DFTRACER_UTILS_LOG_DEBUG(
        "Chunk %d: Extracted %zu events and collected %zu event IDs",
        input.chunk_index, total_events, result.event_ids.size());

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

    return result;
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
