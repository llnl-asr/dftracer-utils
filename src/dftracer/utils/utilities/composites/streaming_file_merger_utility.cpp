#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/utilities/composites/streaming_file_merger_utility.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_compressor_utility.h>
#include <dftracer/utils/utilities/fileio/lines/streaming_line_reader.h>
#include <dftracer/utils/utilities/fileio/streaming_file_writer_utility.h>

#include <utility>

namespace dftracer::utils::utilities::composites {

namespace {

// FNV-1a hash for byte-level verification
inline std::size_t fnv1a_line(const char* data, std::size_t len) {
    std::size_t h = 14695981039346656037ULL;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::size_t>(static_cast<unsigned char>(data[i]));
        h *= 1099511628211ULL;
    }
    return h;
}

// Check if a line is an array delimiter ([ or ]) after trimming whitespace.
inline bool is_array_delimiter(const char* data, std::size_t len) {
    // Skip leading whitespace
    std::size_t start = 0;
    while (start < len &&
           (data[start] == ' ' || data[start] == '\t' || data[start] == '\r')) {
        ++start;
    }
    if (start >= len) return true;  // empty/whitespace-only line
    char ch = data[start];
    return ch == '[' || ch == ']';
}

}  // namespace

// ============================================================================
// Producer: decompress file, strip array delimiters, send raw chunks
// ============================================================================

coro::CoroTask<StreamingFileProducerOutput>
StreamingFileProducerUtility::process_async(
    [[maybe_unused]] CoroScope& ctx, const StreamingFileProducerInput& input) {
    StreamingFileProducerOutput result;
    result.file_path = input.file_path;

    try {
        auto reader_config =
            fileio::lines::StreamingLineReaderConfig().with_file(
                input.file_path);

        auto line_gen =
            fileio::lines::StreamingLineReader::read_async(reader_config);

        const std::size_t batch_budget = input.batch_byte_budget;
        // Acquire a reusable buffer from the pool (zero alloc after warmup)
        std::string local_buf = buf_pool_->acquire();
        local_buf.clear();
        std::size_t batch_events = 0;
        std::size_t batch_hash = 0;
        std::size_t total_hash = 0;

        auto flush = [&]() -> coro::CoroTask<bool> {
            if (batch_events == 0) co_return true;
            StreamingMergeBatch batch;
            batch.buf = std::move(local_buf);
            batch.event_count = batch_events;
            batch.batch_hash = batch_hash;
            total_hash += batch_hash;

            // Acquire next buffer from pool (blocks if all in use)
            local_buf = buf_pool_->acquire();
            local_buf.clear();
            batch_events = 0;
            batch_hash = 0;

            co_return co_await channel_->send(std::move(batch));
        };

        while (auto line_opt = co_await line_gen.next()) {
            const auto& line = *line_opt;
            if (line.content.empty()) continue;

            if (is_array_delimiter(line.content.data(), line.content.size()))
                continue;

            const char* trimmed;
            std::size_t trimmed_length;
            if (!json_trim_and_validate(line.content.data(),
                                        line.content.size(), trimmed,
                                        trimmed_length) ||
                trimmed_length <= 2) {
                continue;
            }

            if (batch_events > 0) local_buf += '\n';
            local_buf.append(trimmed, trimmed_length);
            ++batch_events;
            ++result.events_sent;

            if (input.verify) {
                batch_hash += fnv1a_line(trimmed, trimmed_length);
            }

            if (local_buf.size() >= batch_budget) {
                if (!co_await flush()) co_return result;
            }
        }

        if (!co_await flush()) {
            buf_pool_->release(std::move(local_buf));
            co_return result;
        }

        // Return unused buffer to pool
        buf_pool_->release(std::move(local_buf));

        if (input.verify) {
            result.input_hash = total_hash;
        }

        result.success = true;

    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Producer error for %s: %s",
                                 input.file_path.c_str(), e.what());
    }

    co_return result;
}

// ============================================================================
// Consumer: read raw byte batches from channel, write with array wrapper
// ============================================================================

coro::CoroTask<StreamingFileConsumerOutput>
StreamingFileConsumerUtility::process_async(
    [[maybe_unused]] CoroScope& ctx, const StreamingFileConsumerInput& input) {
    StreamingFileConsumerOutput result;
    result.output_path =
        input.compress ? input.output_file + ".gz" : input.output_file;

    try {
        if (input.compress) {
            // Compressed path: compress via ByteView generator, zero alloc
            fileio::StreamingFileWriterUtility writer(result.output_path);
            compression::zlib::ManualStreamingCompressorUtility compressor;

            auto write_compressed =
                [&](const char* data, std::size_t len) -> coro::CoroTask<void> {
                auto gen = compressor.compress(ByteView(data, len));
                while (auto chunk = co_await gen.next()) {
                    co_await writer.process(*chunk);
                }
            };

            co_await write_compressed("[\n", 2);

            bool first = true;
            while (auto next = co_await ctx.receive(channel_)) {
                auto& current = *next;
                result.output_hash += current.batch_hash;
                result.total_events += current.event_count;
                if (current.buf.empty()) continue;
                if (!first) co_await write_compressed("\n", 1);
                first = false;
                co_await write_compressed(current.buf.data(),
                                          current.buf.size());
                buf_pool_->release(std::move(current.buf));
            }

            if (result.total_events > 0) {
                co_await write_compressed("\n]\n", 3);
            } else {
                co_await write_compressed("]\n", 2);
            }

            auto fin = compressor.finalize_stream();
            while (auto chunk = co_await fin.next()) {
                co_await writer.process(*chunk);
            }
            writer.close();
        } else {
            // Uncompressed path: write directly to ofstream, zero allocs
            std::ofstream ofs(result.output_path, std::ios::binary);
            if (!ofs) {
                throw std::runtime_error("Failed to open output: " +
                                         result.output_path);
            }

            ofs.write("[\n", 2);

            bool first = true;
            while (auto next = co_await ctx.receive(channel_)) {
                auto& current = *next;
                result.output_hash += current.batch_hash;
                result.total_events += current.event_count;
                if (current.buf.empty()) continue;
                if (!first) ofs.write("\n", 1);
                first = false;
                ofs.write(current.buf.data(),
                          static_cast<std::streamsize>(current.buf.size()));
                buf_pool_->release(std::move(current.buf));
            }

            if (result.total_events > 0) {
                ofs.write("\n]\n", 3);
            } else {
                ofs.write("]\n", 2);
            }
            ofs.close();
        }
        result.success = true;

    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_ERROR("Consumer error: %s", e.what());
    }

    co_return result;
}

}  // namespace dftracer::utils::utilities::composites
