#include <dftracer/utils/reader/reader_factory.h>
#include <dftracer/utils/utilities/composites/file_merger.h>
#include <dftracer/utils/utilities/filesystem/directory_scanner.h>
#include <dftracer/utils/utilities/io/file_reader.h>
#include <dftracer/utils/utilities/text/shared.h>

#include <algorithm>
#include <iterator>
#include <sstream>

namespace dftracer::utils::utilities::composites {

// ============================================================================
// FileMergerUtility Implementation
// ============================================================================

std::atomic<int> FileMergerUtility::file_counter_{0};

FileMergerUtilityOutput FileMergerUtility::process(
    const FileMergerUtilityInput& input) {
    FileMergerUtilityOutput result;
    result.file_path = input.file_path;
    result.output_path = input.output_path;

    try {
        // Step 1: Build index if needed for compressed files
        bool is_compressed =
            (input.file_path.size() >= 3 &&
             input.file_path.substr(input.file_path.size() - 3) == ".gz");

        if (is_compressed) {
            // Use IndexBuilderUtility for compressed files
            auto index_input =
                dft::IndexBuildUtilityInput::from_file(input.file_path)
                    .with_index(input.index_path)
                    .with_checkpoint_size(input.checkpoint_size)
                    .with_force_rebuild(input.force_rebuild);

            dft::IndexBuilderUtility index_builder;
            auto index_result = index_builder.process(index_input);

            if (!index_result.success) {
                DFTRACER_UTILS_LOG_ERROR("Failed to build index for %s",
                                         input.file_path.c_str());
                return result;
            }
        }

        // Step 2: Create line processor function that validates JSON
        auto json_validator =
            [](const io::lines::Line& line) -> std::optional<ValidatedEvent> {
            const char* trimmed;
            std::size_t trimmed_length;
            if (json_trim_and_validate(line.content.data(), line.content.size(),
                                       trimmed, trimmed_length) &&
                trimmed_length > 8) {
                ValidatedEvent event;
                event.content = std::string(trimmed, trimmed_length);
                event.line_number = line.line_number;
                return event;
            }

            return std::nullopt;
        };

        // Step 3: Process lines using LineBatchProcessor
        LineBatchProcessorUtility<ValidatedEvent> processor(json_validator);

        io::lines::LineReadInput read_input;
        read_input.file_path = input.file_path;
        if (is_compressed) {
            read_input.idx_path = input.index_path;
        }

        auto validated_events = processor.process(read_input);

        // Step 4: Write validated events to output using StreamingFileWriter
        io::StreamingFileWriterUtility writer(input.output_path, false, true);

        bool first = true;
        for (const auto& event : validated_events) {
            if (!first) {
                io::RawData newline_data{std::vector<unsigned char>{'\n'}};
                writer.process(newline_data);
            }

            io::RawData event_data(event.content);
            writer.process(event_data);

            first = false;
            result.valid_events++;
        }

        // Add trailing newline to ensure proper NDJSON format
        if (!validated_events.empty()) {
            io::RawData newline_data{std::vector<unsigned char>{'\n'}};
            writer.process(newline_data);
        }

        result.lines_processed = validated_events.size();
        result.success = true;

        if (is_compressed) {
            auto reader =
                ReaderFactory::create(input.file_path, input.index_path);
            if (reader) {
                result.total_lines = reader->get_num_lines();
            }
        } else {
            // For plain files, count the lines
            std::ifstream file(input.file_path);
            std::string line;
            std::size_t line_count = 0;
            while (std::getline(file, line)) {
                line_count++;
            }
            result.total_lines = line_count;
        }

        DFTRACER_UTILS_LOG_DEBUG(
            "Processed %s: %zu valid events from %zu total lines",
            input.file_path.c_str(), result.valid_events, result.total_lines);

    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Error processing file %s: %s",
                                 input.file_path.c_str(), e.what());
        result.success = false;
    }

    return result;
}

// ============================================================================
// FileCombinerUtility Implementation
// ============================================================================

FileCombinerUtilityOutput FileCombinerUtility::process(
    const FileCombinerUtilityInput& input) {
    FileCombinerUtilityOutput output;
    output.output_path = input.output_file;

    // Create event extractor for collecting events during merge
    utilities::composites::dft::EventIdExtractor event_extractor;

    try {
        // Step 1: Use StreamingFileWriter to write the combined JSON array
        io::StreamingFileWriterUtility writer(input.output_file, false, true);

        io::RawData array_open(std::vector<unsigned char>{'[', '\n'});
        writer.process(array_open);

        DFTRACER_UTILS_LOG_DEBUG("Processing %zu file results",
                                 input.file_results.size());
        for (const auto& result : input.file_results) {
            DFTRACER_UTILS_LOG_DEBUG(
                "File result: success=%d, output_path='%s', valid_events=%zu",
                result.success, result.output_path.c_str(),
                result.valid_events);
            if (result.success && !result.output_path.empty()) {
                // Use FileReader utility to read the temp file
                utilities::filesystem::FileEntry file_entry{result.output_path};
                utilities::io::FileReaderUtility file_reader;
                utilities::text::Text content_text =
                    file_reader.process(file_entry);

                if (!content_text.content.empty()) {
                    const std::string& content = content_text.content;
                    auto event_extractor_func =
                        [&event_extractor](const io::lines::Line& line)
                        -> std::optional<utilities::composites::dft::EventId> {
                        // Skip empty lines
                        if (line.content.empty()) {
                            return std::nullopt;
                        }

                        std::string json_line = std::string(line.content);

                        if (json_line[0] == '{') {
                            auto extract_input = utilities::composites::dft::
                                EventIdExtractionInput::from_json(json_line);
                            auto event_id =
                                event_extractor.process(extract_input);

                            if (event_id.is_valid()) {
                                return event_id;
                            }
                        }
                        return std::nullopt;
                    };

                    LineBatchProcessorUtility<
                        utilities::composites::dft::EventId>
                        line_processor(event_extractor_func);

                    io::lines::LineReadInput line_input;
                    line_input.file_path = result.output_path;

                    auto extracted_events = line_processor.process(line_input);

                    DFTRACER_UTILS_LOG_DEBUG(
                        "Collected %zu events from temp file %s",
                        extracted_events.size(), result.output_path.c_str());
                    output.collected_events.insert(
                        output.collected_events.end(), extracted_events.begin(),
                        extracted_events.end());

                    // Write NDJSON content directly (Perfetto format: [ +
                    // NDJSON + ])
                    std::vector<unsigned char> content_bytes(content.begin(),
                                                             content.end());
                    io::RawData content_data(content_bytes);
                    writer.process(content_data);

                    output.files_combined++;
                    output.total_events += result.valid_events;
                }
                fs::remove(result.output_path);
            }
        }  // end for loop

        // Write JSON array closing bracket
        io::RawData array_close(std::vector<unsigned char>{'\n', ']', '\n'});
        writer.process(array_close);

        writer.close();

        // Step 2: Compress if requested using FileCompressorUtility
        if (input.compress) {
            auto compress_input =
                FileCompressionUtilityInput::from_file(input.output_file)
                    .with_output(input.output_file + ".gz")
                    .with_compression_level(6);

            FileCompressorUtility compressor;
            auto compress_result = compressor.process(compress_input);

            if (compress_result.success) {
                fs::remove(input.output_file);
                output.output_path = input.output_file + ".gz";
                DFTRACER_UTILS_LOG_INFO("Created compressed output: %s",
                                        output.output_path.c_str());
            } else {
                DFTRACER_UTILS_LOG_WARN(
                    "%s", "Compression failed, keeping uncompressed file");
            }
        }

        output.success = true;

    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Error combining files: %s", e.what());
        output.success = false;
    }

    return output;
}

}  // namespace dftracer::utils::utilities::composites
