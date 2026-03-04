#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/utilities/composites/dft/chunk_extractor_utility.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_compressor_utility.h>
#include <dftracer/utils/utilities/fileio/lines/streaming_line_reader.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>
#include <fcntl.h>
#include <unistd.h>

namespace dftracer::utils::utilities::composites::dft {

namespace compression = dftracer::utils::utilities::compression::zlib;
namespace hash = dftracer::utils::utilities::hash;

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
                              std::to_string(input.chunk_index) +
                              (input.compress ? ".pfw.gz" : ".pfw");

    ChunkExtractorUtilityOutput result;
    result.chunk_index = input.chunk_index;
    result.output_path = output_path;
    result.size_mb = 0.0;
    result.events = 0;
    result.success = false;

    // Compressor is only constructed when compression is requested.
    // unique_ptr keeps it optional without a separate flag.
    std::unique_ptr<compression::ManualStreamingCompressorUtility> compressor;
    if (input.compress) {
        compressor =
            std::make_unique<compression::ManualStreamingCompressorUtility>(
                Z_DEFAULT_COMPRESSION, compression::CompressionFormat::GZIP);
    }

    // Open output file
    ssize_t open_result = co_await dftracer::utils::io::open(
        output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (open_result < 0) {
        DFTRACER_UTILS_LOG_ERROR("Cannot open output file: %s",
                                 output_path.c_str());
        co_return result;
    }
    int output_fd = static_cast<int>(open_result);

    // Write buffer: accumulate event lines and flush in large chunks to
    // reduce async I/O round-trips from ~2M (per-event) to ~hundreds.
    constexpr std::size_t WRITE_BUFFER_SIZE = 256 * 1024;  // 256 KB
    std::vector<char> write_buffer;
    write_buffer.reserve(WRITE_BUFFER_SIZE);

    // JSON array opening
    write_buffer.insert(write_buffer.end(), {'[', '\n'});

    std::size_t total_events = 0;

    std::size_t content_hash = 0;
    hash::HasherUtility hasher;

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
                    write_buffer.insert(write_buffer.end(), trimmed,
                                        trimmed + trimmed_length);
                    write_buffer.push_back('\n');
                    if (write_buffer.size() >= WRITE_BUFFER_SIZE) {
                        co_await flush_buffer(output_fd, write_buffer,
                                              compressor.get());
                    }

                    if (input.compute_hash) {
                        hasher.reset();
                        hasher.update(
                            std::string_view(trimmed, trimmed_length));
                        content_hash += hasher.get_hash().value;
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
                    const char* trimmed;
                    std::size_t trimmed_length;
                    if (json_trim_and_validate(line.content.data(),
                                               line.content.length(), trimmed,
                                               trimmed_length) &&
                        trimmed_length > 8) {
                        write_buffer.insert(write_buffer.end(), trimmed,
                                            trimmed + trimmed_length);
                        write_buffer.push_back('\n');
                        if (write_buffer.size() >= WRITE_BUFFER_SIZE) {
                            co_await flush_buffer(output_fd, write_buffer,
                                                  compressor.get());
                        }

                        if (input.compute_hash) {
                            hasher.reset();
                            hasher.update(
                                std::string_view(trimmed, trimmed_length));
                            content_hash += hasher.get_hash().value;
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
                    const char* trimmed;
                    std::size_t trimmed_length;
                    if (json_trim_and_validate(line.content.data(),
                                               line.content.length(), trimmed,
                                               trimmed_length) &&
                        trimmed_length > 8) {
                        write_buffer.insert(write_buffer.end(), trimmed,
                                            trimmed + trimmed_length);
                        write_buffer.push_back('\n');
                        if (write_buffer.size() >= WRITE_BUFFER_SIZE) {
                            co_await flush_buffer(output_fd, write_buffer,
                                                  compressor.get());
                        }

                        if (input.compute_hash) {
                            hasher.reset();
                            hasher.update(
                                std::string_view(trimmed, trimmed_length));
                            content_hash += hasher.get_hash().value;
                        }

                        total_events++;
                    }
                }
            }
        }
    }

    // JSON array closing + final flush of whatever remains in the buffer
    write_buffer.insert(write_buffer.end(), {']', '\n'});
    co_await flush_buffer(output_fd, write_buffer, compressor.get());

    // Finalize gzip stream and write trailing bytes before closing the fd
    if (compressor) {
        auto final_chunks = compressor->finalize();
        for (const auto& chunk : final_chunks) {
            co_await dftracer::utils::io::write(
                output_fd, reinterpret_cast<const char*>(chunk.data.data()),
                chunk.size());
        }
    }

    co_await dftracer::utils::io::close(output_fd);

    result.events = total_events;
    result.size_mb = input.manifest.total_size_mb;
    result.event_hash = content_hash;
    result.success = true;

    DFTRACER_UTILS_LOG_DEBUG(
        "Chunk %d: %zu events, %.2f MB written to %s (hash=0x%zx)",
        input.chunk_index, result.events, result.size_mb,
        result.output_path.c_str(), result.event_hash);

    co_return result;
}

coro::CoroTask<void> ChunkExtractorUtility::flush_buffer(
    int fd, std::vector<char>& buffer,
    compression::ManualStreamingCompressorUtility* compressor) {
    if (buffer.empty()) co_return;

    if (compressor == nullptr) {
        co_await dftracer::utils::io::write(fd, buffer.data(), buffer.size());
    } else {
        fileio::RawData raw(std::vector<unsigned char>(
            reinterpret_cast<const unsigned char*>(buffer.data()),
            reinterpret_cast<const unsigned char*>(buffer.data()) +
                buffer.size()));
        auto chunks = co_await compressor->process(raw);
        for (const auto& chunk : chunks) {
            co_await dftracer::utils::io::write(
                fd, reinterpret_cast<const char*>(chunk.data.data()),
                chunk.size());
        }
    }
    buffer.clear();
}

coro::CoroTask<void> ChunkExtractorUtility::write_data(
    int fd, const char* data, std::size_t len,
    compression::ManualStreamingCompressorUtility* compressor) {
    if (compressor == nullptr) {
        co_await dftracer::utils::io::write(fd, data, len);
        co_return;
    }

    // Build RawData from the raw bytes without an extra heap allocation for
    // the string: use the vector<unsigned char> constructor directly.
    fileio::RawData raw(std::vector<unsigned char>(
        reinterpret_cast<const unsigned char*>(data),
        reinterpret_cast<const unsigned char*>(data) + len));
    auto chunks = co_await compressor->process(raw);
    for (const auto& chunk : chunks) {
        co_await dftracer::utils::io::write(
            fd, reinterpret_cast<const char*>(chunk.data.data()), chunk.size());
    }
}

}  // namespace dftracer::utils::utilities::composites::dft
