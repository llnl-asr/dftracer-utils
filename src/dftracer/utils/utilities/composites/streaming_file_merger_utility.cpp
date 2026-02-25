#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/utilities/composites/dft/index_builder_utility.h>
#include <dftracer/utils/utilities/composites/file_compressor_utility.h>
#include <dftracer/utils/utilities/composites/streaming_file_merger_utility.h>
#include <dftracer/utils/utilities/fileio/lines/streaming_line_reader.h>
#include <dftracer/utils/utilities/fileio/streaming_file_writer_utility.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>

#include <fstream>
#include <utility>

namespace dftracer::utils::utilities::composites {

coro::CoroTask<StreamingFileProducerOutput>
StreamingFileProducerUtility::process_async(
    [[maybe_unused]] CoroScope& ctx, const StreamingFileProducerInput& input) {
    StreamingFileProducerOutput result;
    result.file_path = input.file_path;

    try {
        bool is_compressed =
            (input.file_path.size() >= 3 &&
             input.file_path.substr(input.file_path.size() - 3) == ".gz");

        if (is_compressed) {
            auto index_input =
                dft::IndexBuildUtilityInput::from_file(input.file_path)
                    .with_index(input.index_path)
                    .with_checkpoint_size(input.checkpoint_size)
                    .with_force_rebuild(input.force_rebuild);

            dft::IndexBuilderUtility index_builder;
            auto index_result = co_await index_builder.process(index_input);

            if (!index_result.success) {
                DFTRACER_UTILS_LOG_ERROR("Failed to build index for %s",
                                         input.file_path.c_str());
                co_return result;
            }
        }

        dft::EventIdExtractor event_extractor;
        dft::IncrementalEventHasher hasher;

        auto reader_config =
            fileio::lines::StreamingLineReaderConfig()
                .with_file(input.file_path)
                .with_index(is_compressed ? input.index_path : "");

        auto line_gen =
            fileio::lines::StreamingLineReader::read_async(reader_config);

        StreamingMergeBatchUtility batch;

        while (auto line_opt = co_await line_gen.next()) {
            const auto& line = *line_opt;
            const char* trimmed;
            std::size_t trimmed_length;

            if (json_trim_and_validate(line.content.data(), line.content.size(),
                                       trimmed, trimmed_length) &&
                trimmed_length > 8) {
                std::string content(trimmed, trimmed_length);

                auto extract_input =
                    dft::EventIdExtractionInput::from_json(content);
                auto event_id = co_await event_extractor.process(extract_input);

                if (event_id.is_valid()) {
                    hasher.update(event_id);
                    batch.add(std::move(content), event_id);
                    result.events_sent++;

                    if (batch.size() >= input.batch_size) {
                        if (!co_await channel_->send(std::move(batch))) {
                            co_return result;
                        }
                        batch = StreamingMergeBatchUtility{};
                    }
                }
            }
        }

        if (!batch.empty()) {
            if (!co_await channel_->send(std::move(batch))) {
                co_return result;
            }
        }

        result.input_hash = hasher.get_hash();
        result.success = true;

        DFTRACER_UTILS_LOG_DEBUG("Producer sent %zu events from %s",
                                 result.events_sent, input.file_path.c_str());

    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Error processing file %s: %s",
                                 input.file_path.c_str(), e.what());
        result.success = false;
    }

    co_return result;
}

coro::CoroTask<StreamingFileConsumerOutput>
StreamingFileConsumerUtility::process_async(
    CoroScope& ctx, const StreamingFileConsumerInput& input) {
    StreamingFileConsumerOutput result;
    result.output_path = input.output_file;

    try {
        fileio::StreamingFileWriterUtility writer(input.output_file, false,
                                                  true);

        fileio::RawData array_open(std::vector<unsigned char>{'[', '\n'});
        co_await writer.process(array_open);

        StreamingMergeBatchUtility batch;
        bool first = true;

        while (auto next = co_await ctx.receive(channel_)) {
            auto& current = *next;
            result.output_hash += current.batch_hash;

            for (auto& content : current.contents) {
                if (!first) {
                    fileio::RawData newline_data{
                        std::vector<unsigned char>{'\n'}};
                    co_await writer.process(newline_data);
                }

                fileio::RawData event_data(std::move(content));
                co_await writer.process(event_data);

                first = false;
                result.total_events++;
            }
        }

        if (result.total_events > 0) {
            fileio::RawData newline_data{std::vector<unsigned char>{'\n'}};
            co_await writer.process(newline_data);
        }

        fileio::RawData array_close(std::vector<unsigned char>{']', '\n'});
        co_await writer.process(array_close);

        writer.close();

        if (input.compress) {
            auto compress_input =
                FileCompressionUtilityInput::from_file(input.output_file)
                    .with_output(input.output_file + ".gz")
                    .with_compression_level(6);

            FileCompressorUtility compressor;
            auto compress_result = co_await compressor.process(compress_input);

            if (compress_result.success) {
                fs::remove(input.output_file);
                result.output_path = input.output_file + ".gz";
            }
        }

        result.success = true;

        DFTRACER_UTILS_LOG_INFO("Consumer wrote %zu events to %s",
                                result.total_events,
                                result.output_path.c_str());

    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Error writing output: %s", e.what());
        result.success = false;
    }

    co_return result;
}

}  // namespace dftracer::utils::utilities::composites
