#include <dftracer/utils/core/common/archive_format.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_pruner_utility.h>
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
#include <yyjson.h>

#include <cstring>
#include <optional>
#include <span>

namespace dftracer::utils::utilities::reader {

namespace dft_internal = composites::dft::internal;
using common::json::JsonValue;
using common::query::Query;
using composites::dft::indexing::ChunkPrunerInput;
using composites::dft::indexing::ChunkPrunerUtility;
using indexer::internal::IndexerFactory;

namespace {

bool line_matches_query(const Query& q, std::string_view content) {
    yyjson_doc* doc = yyjson_read(content.data(), content.size(), 0);
    if (!doc) return false;
    yyjson_val* root = yyjson_doc_get_root(doc);
    bool result = false;
    if (root && yyjson_is_obj(root)) {
        JsonValue json(root);
        result = q.evaluate(json);
    }
    yyjson_doc_free(doc);
    return result;
}

}  // namespace

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

void TraceReader::ensure_metadata_cached() {
    if (metadata_cached_) return;

    if (has_index_) {
        auto reader = create_indexed_reader();
        cached_max_bytes_ = reader->get_max_bytes();
        cached_num_lines_ = reader->get_num_lines();
    } else if (format_ == ArchiveFormat::GZIP ||
               format_ == ArchiveFormat::TAR_GZ) {
        cached_max_bytes_ = 0;
        cached_num_lines_ = 0;
    } else {
        std::error_code ec;
        auto size = fs::file_size(config_.file_path, ec);
        cached_max_bytes_ = ec ? 0 : static_cast<std::size_t>(size);
        cached_num_lines_ = 0;
    }
    metadata_cached_ = true;
}

std::size_t TraceReader::get_max_bytes() {
    ensure_metadata_cached();
    return cached_max_bytes_;
}

std::size_t TraceReader::get_num_lines() {
    ensure_metadata_cached();
    return cached_num_lines_;
}

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
    std::optional<Query> query;
    if (!config.query.empty()) {
        auto parsed = Query::from_string(config.query);
        if (!parsed) throw common::query::QueryParseError(parsed.error());
        query = std::move(*parsed);
    }

    if (has_index_) {
        auto reader = create_indexed_reader();
        auto range_type = resolve_range_type(config);
        std::size_t start =
            config.has_line_range() ? config.start_line : config.start_byte;
        std::size_t end =
            config.has_line_range() ? config.end_line : config.end_byte;

        if (range_type == internal::RangeType::LINE_RANGE) {
            auto total_lines = reader->get_num_lines();
            if (start == 0) start = 1;
            if (end == 0 || end > total_lines) end = total_lines;
            if (start > total_lines) co_return;
        } else {
            auto max_bytes = reader->get_max_bytes();
            if (end == 0 || end > max_bytes) end = max_bytes;
            if (start >= max_bytes) co_return;
        }

        if (query && !idx_path_.empty() &&
            range_type == internal::RangeType::BYTE_RANGE) {
            ChunkPrunerInput pruner_input{idx_path_, config_.file_path, *query,
                                          nullptr};
            ChunkPrunerUtility pruner;
            auto pruner_out = co_await pruner.process(pruner_input);
            if (pruner_out.success && !pruner_out.file_may_match) {
                co_return;
            }
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
                    auto line_sv = std::string_view(data + pos, end_pos - pos);
                    if (!query || line_matches_query(*query, line_sv)) {
                        co_yield Line(line_sv, line_num);
                    }
                    ++line_num;
                } else {
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
            if (!query || line_matches_query(*query, opt->content)) {
                co_yield *opt;
            }
        }
    } else {
        std::size_t start = config.has_line_range() ? config.start_line : 0;
        std::size_t end = config.has_line_range() ? config.end_line : 0;
        auto gen = fileio::lines::sources::async_plain_file_lines(
            config_.file_path, start, end);
        while (auto opt = co_await gen.next()) {
            if (!query || line_matches_query(*query, opt->content)) {
                co_yield *opt;
            }
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

        if (range_type == internal::RangeType::LINE_RANGE) {
            auto total_lines = reader->get_num_lines();
            if (start == 0) start = 1;
            if (end == 0 || end > total_lines) end = total_lines;
            if (start > total_lines) co_return;
        } else {
            auto max_bytes = reader->get_max_bytes();
            if (end == 0 || end > max_bytes) end = max_bytes;
            if (start >= max_bytes) co_return;
        }

        if (!config.query.empty() && !idx_path_.empty() &&
            range_type == internal::RangeType::BYTE_RANGE) {
            auto parsed = Query::from_string(config.query);
            if (!parsed) throw common::query::QueryParseError(parsed.error());
            ChunkPrunerInput pruner_input{idx_path_, config_.file_path,
                                          std::move(*parsed), nullptr};
            ChunkPrunerUtility pruner;
            auto pruner_out = co_await pruner.process(pruner_input);
            if (pruner_out.success && !pruner_out.file_may_match) {
                co_return;
            }
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
