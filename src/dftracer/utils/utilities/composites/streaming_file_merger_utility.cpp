#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/utilities/composites/streaming_file_merger_utility.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_compressor_utility.h>
#include <dftracer/utils/utilities/fileio/lines/streaming_line_reader.h>
#include <dftracer/utils/utilities/fileio/streaming_file_writer_utility.h>

#include <fstream>
#include <utility>

namespace dftracer::utils::utilities::composites {

coro::CoroTask<StreamingFileProducerOutput>
StreamingFileProducerUtility::process_async(
    [[maybe_unused]] CoroScope& ctx, const StreamingFileProducerInput& input) {
    StreamingFileProducerOutput result;
    result.file_path = input.file_path;

    try {
        // Merge always reads sequentially, so we never need a sidecar
        // index.  Stream-decompress .gz files directly in a single pass.
        auto reader_config =
            fileio::lines::StreamingLineReaderConfig().with_file(
                input.file_path);

        auto line_gen =
            fileio::lines::StreamingLineReader::read_async(reader_config);

        StreamingMergeBatchUtility batch;

        if (input.verify) {
            dft::EventIdExtractor event_extractor;
            dft::IncrementalEventHasher hasher;

            while (auto line_opt = co_await line_gen.next()) {
                const auto& line = *line_opt;
                const char* trimmed;
                std::size_t trimmed_length;

                if (json_trim_and_validate(line.content.data(),
                                           line.content.size(), trimmed,
                                           trimmed_length) &&
                    trimmed_length > 8) {
                    std::string content(trimmed, trimmed_length);

                    auto extract_input =
                        dft::EventIdExtractionInput::from_json(content);
                    auto event_id =
                        co_await event_extractor.process(extract_input);

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

            result.input_hash = hasher.get_hash();
        } else {
            // Fast path: skip JSON parsing and hashing, include all
            // valid JSON lines (events + metadata).
            while (auto line_opt = co_await line_gen.next()) {
                const auto& line = *line_opt;
                const char* trimmed;
                std::size_t trimmed_length;

                if (json_trim_and_validate(line.content.data(),
                                           line.content.size(), trimmed,
                                           trimmed_length) &&
                    trimmed_length > 8) {
                    batch.add_unchecked(std::string(trimmed, trimmed_length));
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

    try {
        const std::string out_path =
            input.compress ? input.output_file + ".gz" : input.output_file;
        result.output_path = out_path;

        fileio::StreamingFileWriterUtility writer(out_path, false, true);

        std::unique_ptr<compression::zlib::ManualStreamingCompressorUtility>
            compressor;
        if (input.compress) {
            compressor = std::make_unique<
                compression::zlib::ManualStreamingCompressorUtility>(
                6, compression::zlib::CompressionFormat::GZIP);
        }

        // Write a chunk, routing through the compressor when active.
        auto write_chunk =
            [&](const fileio::RawData& raw) -> coro::CoroTask<void> {
            if (compressor) {
                auto chunks = co_await compressor->process(raw);
                for (const auto& chunk : chunks) {
                    co_await writer.process(fileio::RawData{chunk.data});
                }
            } else {
                co_await writer.process(raw);
            }
        };

        co_await write_chunk(fileio::RawData(std::string{"[\n"}));

        bool first = true;

        while (auto next = co_await ctx.receive(channel_)) {
            auto& current = *next;
            result.output_hash += current.batch_hash;

            // Pre-calculate buffer size: each event + leading '\n' (except
            // the very first event overall).
            std::size_t total = 0;
            for (const auto& s : current.contents) {
                total += s.size() + 1;  // +1 for '\n'
            }
            if (first && !current.contents.empty()) {
                total -= 1;  // no leading newline before the first event
            }

            std::string buf;
            buf.reserve(total);

            for (const auto& s : current.contents) {
                if (!first) {
                    buf += '\n';
                }
                buf += s;
                first = false;
                result.total_events++;
            }

            co_await write_chunk(fileio::RawData(buf));
        }

        if (result.total_events > 0) {
            co_await write_chunk(fileio::RawData(std::string{"\n]\n"}));
        } else {
            co_await write_chunk(fileio::RawData(std::string{"]\n"}));
        }

        if (compressor) {
            auto final_chunks = compressor->finalize();
            for (const auto& chunk : final_chunks) {
                co_await writer.process(fileio::RawData{chunk.data});
            }
        }

        writer.close();
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
