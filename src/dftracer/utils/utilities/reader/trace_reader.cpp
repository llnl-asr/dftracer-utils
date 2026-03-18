#include <dftracer/utils/core/common/archive_format.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_plain_file_line_generator.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>
#include <dftracer/utils/utilities/reader/internal/stream.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <dftracer/utils/utilities/reader/internal/stream_type.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>

#include <cstring>
#include <span>

namespace dftracer::utils::utilities::reader {

namespace dft_internal = composites::dft::internal;
using indexer::internal::IndexerFactory;

TraceReader::TraceReader(TraceReaderConfig config)
    : config_(std::move(config)) {
    probe_index();
}

void TraceReader::probe_index() {
    idx_path_ = dft_internal::determine_index_path(config_.file_path,
                                                   config_.index_dir);
    has_index_ = fs::exists(idx_path_);
    format_ = IndexerFactory::detect_format(config_.file_path);
}

bool TraceReader::has_index() const { return has_index_; }

std::shared_ptr<internal::Reader> TraceReader::create_indexed_reader() {
    auto indexer = IndexerFactory::create(config_.file_path, idx_path_,
                                          config_.checkpoint_size, false);
    return internal::ReaderFactory::create(indexer);
}

internal::StreamType TraceReader::resolve_raw_stream_type(
    const ReadConfig& config) const {
    if (!config.line_aligned) return internal::StreamType::BYTES;
    if (config.multi_line) return internal::StreamType::MULTI_LINES_BYTES;
    return internal::StreamType::LINE_BYTES;
}

internal::RangeType TraceReader::resolve_range_type(
    const ReadConfig& config) const {
    if (config.has_line_range()) return internal::RangeType::LINE_RANGE;
    return internal::RangeType::BYTE_RANGE;
}

coro::AsyncGenerator<Line> TraceReader::read_lines(ReadConfig config) {
    if (has_index_) {
        auto reader = create_indexed_reader();
        auto range_type = resolve_range_type(config);
        std::size_t start =
            config.has_line_range() ? config.start_line : config.start_byte;
        std::size_t end =
            config.has_line_range() ? config.end_line : config.end_byte;

        if (end == 0 && range_type == internal::RangeType::LINE_RANGE) {
            end = reader->get_num_lines();
        }
        if (end == 0 && range_type == internal::RangeType::BYTE_RANGE) {
            end = reader->get_max_bytes();
        }
        if (start == 0 && range_type == internal::RangeType::LINE_RANGE) {
            start = 1;
        }

        auto stream =
            reader->stream(internal::StreamConfig()
                               .stream_type(internal::StreamType::MULTI_LINES)
                               .range_type(range_type)
                               .from(start)
                               .to(end)
                               .buffer_size(config.buffer_size));

        std::size_t line_num = start;
        while (!stream->done()) {
            auto chunk = co_await stream->read_async();
            if (chunk.empty()) break;
            const char* data = chunk.data();
            std::size_t len = chunk.size();
            std::size_t pos = 0;
            while (pos < len) {
                const void* nl_ptr = std::memchr(data + pos, '\n', len - pos);
                std::size_t end_pos =
                    nl_ptr ? static_cast<const char*>(nl_ptr) - data : len;
                if (end_pos > pos) {
                    co_yield Line(std::string_view(data + pos, end_pos - pos),
                                  line_num++);
                } else {
                    // Empty line — still advance line counter
                    ++line_num;
                }
                pos = end_pos + 1;
            }
        }
    } else if (format_ == ArchiveFormat::GZIP ||
               format_ == ArchiveFormat::TAR_GZ) {
        std::size_t start = config.has_line_range() ? config.start_line : 0;
        std::size_t end = config.has_line_range() ? config.end_line : 0;
        auto gen = fileio::lines::sources::async_streaming_gz_lines(
            config_.file_path, start, end);
        while (auto opt = co_await gen.next()) {
            co_yield *opt;
        }
    } else {
        std::size_t start = config.has_line_range() ? config.start_line : 0;
        std::size_t end = config.has_line_range() ? config.end_line : 0;
        auto gen = fileio::lines::sources::async_plain_file_lines(
            config_.file_path, start, end);
        while (auto opt = co_await gen.next()) {
            co_yield *opt;
        }
    }
}

coro::AsyncGenerator<std::span<const char>> TraceReader::read_raw(
    ReadConfig config) {
    if (has_index_) {
        auto reader = create_indexed_reader();
        auto stream_type = resolve_raw_stream_type(config);
        auto range_type = resolve_range_type(config);
        std::size_t start =
            config.has_line_range() ? config.start_line : config.start_byte;
        std::size_t end =
            config.has_line_range() ? config.end_line : config.end_byte;

        if (end == 0 && range_type == internal::RangeType::LINE_RANGE) {
            end = reader->get_num_lines();
        }
        if (end == 0 && range_type == internal::RangeType::BYTE_RANGE) {
            end = reader->get_max_bytes();
        }
        if (start == 0 && range_type == internal::RangeType::LINE_RANGE) {
            start = 1;
        }

        auto stream = reader->stream(internal::StreamConfig()
                                         .stream_type(stream_type)
                                         .range_type(range_type)
                                         .from(start)
                                         .to(end)
                                         .buffer_size(config.buffer_size));

        while (!stream->done()) {
            auto chunk = co_await stream->read_async();
            if (chunk.empty()) break;
            co_yield chunk;
        }
    } else if (format_ == ArchiveFormat::GZIP ||
               format_ == ArchiveFormat::TAR_GZ) {
        auto gen =
            fileio::lines::sources::async_streaming_gz_lines(config_.file_path);
        std::size_t byte_pos = 0;
        while (auto opt = co_await gen.next()) {
            const auto& line = *opt;
            std::size_t line_end = byte_pos + line.content.size() + 1;
            if (config.end_byte > 0 && byte_pos >= config.end_byte) break;
            if (line_end > config.start_byte) {
                co_yield std::span<const char>(line.content.data(),
                                               line.content.size());
            }
            byte_pos = line_end;
        }
    } else {
        auto gen =
            fileio::lines::sources::async_plain_file_lines(config_.file_path);
        std::size_t byte_pos = 0;
        while (auto opt = co_await gen.next()) {
            const auto& line = *opt;
            std::size_t line_end = byte_pos + line.content.size() + 1;
            if (config.end_byte > 0 && byte_pos >= config.end_byte) break;
            if (line_end > config.start_byte) {
                co_yield std::span<const char>(line.content.data(),
                                               line.content.size());
            }
            byte_pos = line_end;
        }
    }
}

}  // namespace dftracer::utils::utilities::reader
