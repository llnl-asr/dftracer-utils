#include <dftracer/utils/core/common/archive_format.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_pruner_utility.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_plain_file_bytes_generator.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_plain_file_line_generator.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>
#include <dftracer/utils/utilities/reader/internal/stream.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <dftracer/utils/utilities/reader/internal/stream_type.h>
#include <dftracer/utils/utilities/reader/internal/trace_reader_prefilter.h>
#include <dftracer/utils/utilities/reader/internal/trace_reader_shared.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <simdjson.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>

namespace dftracer::utils::utilities::reader {

namespace dft_internal = composites::dft::internal;
using common::json::JsonValue;
using common::query::Query;
using composites::dft::indexing::ChunkPrunerInput;
using composites::dft::indexing::ChunkPrunerUtility;
using indexer::internal::IndexerFactory;

using internal::build_prefilter;
using internal::LinePrefilter;
using internal::ondemand_to_literal;
using internal::read_chunks_indexed;
using internal::strip_ndjson_bookends;

namespace {

thread_local simdjson::dom::parser tl_parser;

bool line_matches_query(const Query& q, std::string_view content) {
    auto result = tl_parser.parse(content.data(), content.size());
    if (result.error()) return false;
    auto root = result.value_unsafe();
    if (!root.is_object()) return false;
    JsonValue json(root);
    return q.evaluate(json);
}

struct LineRange {
    std::size_t start_line;
    std::size_t end_line;
};

coro::AsyncGenerator<Line> yield_lines_from_stream(
    std::unique_ptr<internal::ReaderStream> stream, std::size_t start_line_num,
    const Query* query, bool chunk_prune_only = false,
    const LinePrefilter* prefilter = nullptr) {
    std::size_t line_num = start_line_num;
    while (!stream->done()) {
        auto chunk = co_await stream->read_async();
        if (chunk.empty()) break;
        const char* data = chunk.data();
        std::size_t len = chunk.size();

        // Chunk-level pre-filter: if any required literal is absent from this
        // entire buffer, no line within it can match. Skip without splitting.
        // Line numbers must stay correct for subsequent chunks.
        if (prefilter && !prefilter->empty() &&
            !prefilter->may_match(std::string_view(data, len))) {
            line_num += std::count(data, data + len, '\n');
            continue;
        }

        std::size_t pos = 0;
        while (pos < len) {
            const void* nl_ptr = std::memchr(data + pos, '\n', len - pos);
            std::size_t end_pos =
                nl_ptr ? static_cast<const char*>(nl_ptr) - data : len;
            if (end_pos > pos) {
                auto line_sv = std::string_view(data + pos, end_pos - pos);
                bool accept = chunk_prune_only || !query ||
                              line_matches_query(*query, line_sv);
                if (accept && prefilter && !prefilter->empty() &&
                    !prefilter->may_match(line_sv)) {
                    accept = false;
                }
                if (accept) {
                    co_yield Line(line_sv, line_num);
                }
                ++line_num;
            } else {
                ++line_num;
            }
            pos = end_pos + 1;
        }
    }
}

coro::AsyncGenerator<Line> yield_lines_from_ranges(
    std::shared_ptr<internal::Reader> reader, std::vector<LineRange> ranges,
    std::size_t buffer_size, Query query, bool chunk_prune_only = false,
    LinePrefilter prefilter = {}) {
    for (const auto& range : ranges) {
        auto stream =
            reader->stream(internal::StreamConfig()
                               .stream_type(internal::StreamType::MULTI_LINES)
                               .range_type(internal::RangeType::LINE_RANGE)
                               .from(range.start_line)
                               .to(range.end_line)
                               .buffer_size(buffer_size));
        auto gen =
            yield_lines_from_stream(std::move(stream), range.start_line, &query,
                                    chunk_prune_only, &prefilter);
        while (auto line = co_await gen.next()) {
            co_yield *line;
        }
    }
}

// Raw-chunk variants of the yield/read helpers. Same pruning logic as the
// line-yielding flavors but emit std::span<const char> buffers untouched
// (multi-line boundary respected by stream type). Used by read_json to run
// simdjson iterate_many over each chunk instead of parsing line by line.
coro::AsyncGenerator<std::span<const char>> yield_chunks_from_stream(
    std::unique_ptr<internal::ReaderStream> stream,
    const LinePrefilter* prefilter = nullptr) {
    while (!stream->done()) {
        auto chunk = co_await stream->read_async();
        if (chunk.empty()) break;
        if (prefilter && !prefilter->empty() &&
            !prefilter->may_match(
                std::string_view(chunk.data(), chunk.size()))) {
            continue;
        }
        co_yield chunk;
    }
}

coro::AsyncGenerator<std::span<const char>> yield_chunks_from_ranges(
    std::shared_ptr<internal::Reader> reader, std::vector<LineRange> ranges,
    std::size_t buffer_size, LinePrefilter prefilter = {}) {
    for (const auto& range : ranges) {
        auto stream =
            reader->stream(internal::StreamConfig()
                               .stream_type(internal::StreamType::MULTI_LINES)
                               .range_type(internal::RangeType::LINE_RANGE)
                               .from(range.start_line)
                               .to(range.end_line)
                               .buffer_size(buffer_size));
        auto gen = yield_chunks_from_stream(std::move(stream), &prefilter);
        while (auto chunk = co_await gen.next()) {
            co_yield *chunk;
        }
    }
}

coro::AsyncGenerator<Line> read_lines_indexed(
    std::shared_ptr<internal::Reader> reader, std::string index_path,
    std::string file_path, ReadConfig config, std::optional<Query> query,
    bool chunk_prune_only = false) {
    // Keep RocksDB alive for the generator's lifetime so per-method opens
    // in GzipIndexer reuse DBManager's cached handle.
    std::optional<indexer::IndexDatabase> db_keep_alive;
    if (!index_path.empty()) {
        try {
            db_keep_alive.emplace(index_path,
                                  rocksdb::RocksDatabase::OpenMode::ReadOnly);
        } catch (...) {
        }
    }

    LinePrefilter prefilter = query ? build_prefilter(*query) : LinePrefilter{};
    auto range_type = config.has_line_range() ? internal::RangeType::LINE_RANGE
                                              : internal::RangeType::BYTE_RANGE;
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

    if (query && !index_path.empty() &&
        range_type == internal::RangeType::BYTE_RANGE) {
        ChunkPrunerInput pruner_input{index_path, file_path, *query, nullptr};
        ChunkPrunerUtility pruner;
        auto pruner_out = co_await pruner.process(pruner_input);
        if (pruner_out.success && !pruner_out.file_may_match) {
            co_return;
        }

        if (pruner_out.success && !pruner_out.candidate_checkpoints.empty() &&
            pruner_out.candidate_checkpoints.size() <
                pruner_out.total_checkpoints) {
            indexer::IndexDatabase idx_db(
                index_path, rocksdb::RocksDatabase::OpenMode::ReadOnly);
            auto logical = indexer::internal::get_logical_path(file_path);
            int fid = idx_db.get_file_info_id(logical);

            if (fid >= 0) {
                auto all_ckpts = idx_db.query_checkpoints(fid);
                std::unordered_map<std::uint64_t, indexer::IndexerCheckpoint>
                    ckpt_map;
                for (auto& ckpt : all_ckpts) {
                    ckpt_map.emplace(ckpt.checkpoint_idx, std::move(ckpt));
                }

                std::vector<LineRange> ranges;
                std::uint64_t prev_idx = UINT64_MAX;

                for (auto ckpt_idx : pruner_out.candidate_checkpoints) {
                    auto it = ckpt_map.find(ckpt_idx);
                    if (it == ckpt_map.end()) continue;
                    const auto& ckpt = it->second;

                    if (ranges.empty() || ckpt_idx != prev_idx + 1) {
                        ranges.push_back(
                            {ckpt.first_line_num, ckpt.last_line_num});
                    } else {
                        ranges.back().end_line = ckpt.last_line_num;
                    }
                    prev_idx = ckpt_idx;
                }

                auto gen = yield_lines_from_ranges(reader, std::move(ranges),
                                                   config.buffer_size, *query,
                                                   chunk_prune_only, prefilter);
                while (auto line = co_await gen.next()) {
                    co_yield *line;
                }
                co_return;
            }
        }
    }

    auto stream =
        reader->stream(internal::StreamConfig()
                           .stream_type(internal::StreamType::MULTI_LINES)
                           .range_type(range_type)
                           .from(start)
                           .to(end)
                           .buffer_size(config.buffer_size));

    auto gen = yield_lines_from_stream(std::move(stream), start,
                                       query ? &*query : nullptr,
                                       chunk_prune_only, &prefilter);
    while (auto line = co_await gen.next()) {
        co_yield *line;
    }
}

coro::AsyncGenerator<Line> read_lines_gz(std::string file_path,
                                         ReadConfig config,
                                         std::optional<Query> query,
                                         bool chunk_prune_only = false) {
    std::size_t start = config.has_line_range() ? config.start_line : 0;
    std::size_t end = config.has_line_range() ? config.end_line : 0;
    auto gen =
        fileio::lines::sources::async_streaming_gz_lines(file_path, start, end);
    while (auto opt = co_await gen.next()) {
        if (chunk_prune_only || !query ||
            line_matches_query(*query, opt->content)) {
            co_yield *opt;
        }
    }
}

coro::AsyncGenerator<Line> read_lines_plain_bytes(
    std::string file_path, ReadConfig config, std::optional<Query> query,
    bool chunk_prune_only = false) {
    auto gen = fileio::lines::sources::async_plain_file_bytes(
        file_path, config.start_byte, config.end_byte, config.buffer_size);
    while (auto opt = co_await gen.next()) {
        if (chunk_prune_only || !query ||
            line_matches_query(*query, opt->content)) {
            co_yield *opt;
        }
    }
}

coro::AsyncGenerator<Line> read_lines_plain(std::string file_path,
                                            ReadConfig config,
                                            std::optional<Query> query,
                                            bool chunk_prune_only = false) {
    std::size_t start = config.has_line_range() ? config.start_line : 0;
    std::size_t end = config.has_line_range() ? config.end_line : 0;
    auto gen =
        fileio::lines::sources::async_plain_file_lines(file_path, start, end);
    while (auto opt = co_await gen.next()) {
        if (chunk_prune_only || !query ||
            line_matches_query(*query, opt->content)) {
            co_yield *opt;
        }
    }
}

}  // namespace

namespace internal {

coro::AsyncGenerator<std::span<const char>> read_chunks_indexed(
    std::shared_ptr<internal::Reader> reader, std::string index_path,
    std::string file_path, ReadConfig config, std::optional<Query> query,
    bool extend_to_line_boundary) {
    // Keep RocksDB alive for the generator's lifetime so per-method opens
    // in GzipIndexer reuse DBManager's cached handle.
    std::optional<indexer::IndexDatabase> db_keep_alive;
    if (!index_path.empty()) {
        try {
            db_keep_alive.emplace(index_path,
                                  rocksdb::RocksDatabase::OpenMode::ReadOnly);
        } catch (...) {
        }
    }

    LinePrefilter prefilter = query ? build_prefilter(*query) : LinePrefilter{};
    auto range_type = config.has_line_range() ? internal::RangeType::LINE_RANGE
                                              : internal::RangeType::BYTE_RANGE;
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

    if (query && !index_path.empty() && !config.skip_pruning) {
        ChunkPrunerInput pruner_input{index_path, file_path, *query, nullptr};
        ChunkPrunerUtility pruner;
        auto pruner_out = co_await pruner.process(pruner_input);
        if (pruner_out.success && !pruner_out.file_may_match) {
            co_return;
        }

        if (pruner_out.success && !pruner_out.candidate_checkpoints.empty() &&
            pruner_out.candidate_checkpoints.size() <
                pruner_out.total_checkpoints) {
            indexer::IndexDatabase idx_db(
                index_path, rocksdb::RocksDatabase::OpenMode::ReadOnly);
            auto logical = indexer::internal::get_logical_path(file_path);
            int fid = idx_db.get_file_info_id(logical);

            if (fid >= 0) {
                auto all_ckpts = idx_db.query_checkpoints(fid);
                std::unordered_map<std::uint64_t, indexer::IndexerCheckpoint>
                    ckpt_map;
                for (auto& ckpt : all_ckpts) {
                    ckpt_map.emplace(ckpt.checkpoint_idx, std::move(ckpt));
                }

                std::vector<LineRange> ranges;
                std::uint64_t prev_idx = UINT64_MAX;
                for (auto ckpt_idx : pruner_out.candidate_checkpoints) {
                    auto it = ckpt_map.find(ckpt_idx);
                    if (it == ckpt_map.end()) continue;
                    const auto& ckpt = it->second;
                    // Intersect with the caller's window (byte or line) so
                    // checkpoint-level parallel work items stay disjoint.
                    if (range_type == internal::RangeType::BYTE_RANGE) {
                        std::size_t ckpt_start = ckpt.uc_offset;
                        std::size_t ckpt_end = ckpt.uc_offset + ckpt.uc_size;
                        if (ckpt_end <= start) continue;
                        if (ckpt_start >= end) continue;
                    } else {
                        if (ckpt.last_line_num < start) continue;
                        if (ckpt.first_line_num > end) continue;
                    }
                    if (ranges.empty() || ckpt_idx != prev_idx + 1) {
                        ranges.push_back(
                            {ckpt.first_line_num, ckpt.last_line_num});
                    } else {
                        ranges.back().end_line = ckpt.last_line_num;
                    }
                    prev_idx = ckpt_idx;
                }

                if (ranges.empty()) {
                    co_return;
                }

                auto gen = yield_chunks_from_ranges(
                    reader, std::move(ranges), config.buffer_size, prefilter);
                while (auto chunk = co_await gen.next()) {
                    co_yield *chunk;
                }
                co_return;
            }
        }
    }

    auto stream_type = (range_type == internal::RangeType::BYTE_RANGE)
                           ? internal::StreamType::MULTI_LINES_BYTES
                           : internal::StreamType::MULTI_LINES;
    auto stream =
        reader->stream(internal::StreamConfig()
                           .stream_type(stream_type)
                           .range_type(range_type)
                           .from(start)
                           .to(end)
                           .buffer_size(config.buffer_size)
                           .extend_to_line_boundary(
                               extend_to_line_boundary &&
                               range_type == internal::RangeType::BYTE_RANGE));

    auto gen = yield_chunks_from_stream(std::move(stream), &prefilter);
    while (auto chunk = co_await gen.next()) {
        co_yield *chunk;
    }
}

}  // namespace internal

TraceReader::TraceReader(TraceReaderConfig config)
    : config_(std::move(config)) {
    probe_index();
}

void TraceReader::probe_index() {
    format_ = IndexerFactory::detect_format(config_.file_path);
    index_path_ = dft_internal::determine_index_path(config_.file_path,
                                                     config_.index_dir);
    has_index_ =
        (format_ == ArchiveFormat::GZIP || format_ == ArchiveFormat::TAR_GZ) &&
        fs::exists(index_path_);
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
    auto indexer = IndexerFactory::create(config_.file_path, index_path_,
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

    bool cpo = config.chunk_prune_only;

    if (has_index_) {
        return read_lines_indexed(create_indexed_reader(), index_path_,
                                  config_.file_path, std::move(config),
                                  std::move(query), cpo);
    }
    if (format_ == ArchiveFormat::GZIP || format_ == ArchiveFormat::TAR_GZ) {
        return read_lines_gz(config_.file_path, std::move(config),
                             std::move(query), cpo);
    }
    if (config.has_byte_range()) {
        return read_lines_plain_bytes(config_.file_path, std::move(config),
                                      std::move(query), cpo);
    }
    return read_lines_plain(config_.file_path, std::move(config),
                            std::move(query), cpo);
}

coro::AsyncGenerator<JsonLine> TraceReader::read_json(ReadConfig config) {
    std::optional<Query> query;
    if (!config.query.empty()) {
        auto parsed = Query::from_string(config.query);
        if (!parsed) throw common::query::QueryParseError(parsed.error());
        query = std::move(*parsed);
    }

    // chunk_prune_only path: dim_stats already proved every event with the
    // predicate field matches; we still need to skip events lacking the
    // field (e.g., metadata "ph":"M" events). Field-presence probe is
    // cheaper than full ValueMap eval.
    std::vector<std::string> presence_check_paths;
    if (query && config.chunk_prune_only) {
        const auto& fset = query->fields();
        presence_check_paths.assign(fset.begin(), fset.end());
    }

    // Fast path: indexed gz files go through a chunk generator with
    // simdjson iterate_many. Query is evaluated on the ondemand document
    // directly, so non-matching docs never hit the yield_parser.
    if (has_index_) {
        auto reader = create_indexed_reader();
        auto chunk_gen = read_chunks_indexed(reader, index_path_,
                                             config_.file_path, config, query);

        simdjson::ondemand::parser bulk_parser;
        common::json::JsonParser yield_parser;

        while (auto chunk_opt = co_await chunk_gen.next()) {
            auto chunk = *chunk_opt;
            if (chunk.empty()) continue;
            auto trimmed = strip_ndjson_bookends(
                std::string_view(chunk.data(), chunk.size()));
            if (trimmed.empty()) continue;
            simdjson::padded_string padded(trimmed);

            auto docs_r = bulk_parser.iterate_many(
                padded, 1 << 20, /*allow_comma_separated=*/false);
            if (docs_r.error()) continue;
            auto& docs = docs_r.value();

            for (auto it = docs.begin(); it != docs.end(); ++it) {
                auto doc_result = *it;
                if (doc_result.error()) continue;
                auto& doc = doc_result.value();

                std::string_view src(it.source().data(), it.source().size());

                if (query && config.chunk_prune_only) {
                    bool all_present = true;
                    for (const auto& path : presence_check_paths) {
                        auto fld = doc.find_field_unordered(path);
                        if (fld.error()) {
                            all_present = false;
                            break;
                        }
                    }
                    if (!all_present) continue;
                    doc.rewind();
                } else if (query) {
                    common::query::ValueMap fields;
                    auto obj = doc.get_object();
                    if (obj.error()) continue;
                    for (auto field : obj.value()) {
                        if (field.error()) continue;
                        auto key_r = field.unescaped_key();
                        if (key_r.error()) continue;
                        auto val_r = field.value();
                        if (val_r.error()) continue;
                        auto key = key_r.value();
                        auto val = val_r.value();
                        auto type_r = val.type();
                        if (type_r.error()) continue;
                        auto type = type_r.value();
                        if (type == simdjson::ondemand::json_type::object) {
                            auto nested = val.get_object();
                            if (nested.error()) continue;
                            for (auto nf : nested.value()) {
                                if (nf.error()) continue;
                                auto nk_r = nf.unescaped_key();
                                if (nk_r.error()) continue;
                                auto nv_r = nf.value();
                                if (nv_r.error()) continue;
                                auto nk = nk_r.value();
                                if (!query->references(nk)) continue;
                                fields[std::string(nk)] =
                                    ondemand_to_literal(nv_r.value());
                            }
                        } else if (query->references(key)) {
                            fields[std::string(key)] = ondemand_to_literal(val);
                        }
                    }
                    if (!query->evaluate(fields)) continue;
                }

                // Matched (or no query): lend the iterate_many doc to
                // yield_parser without re-parsing. Consumers like
                // build_arrow_row call parser.for_each_field which now
                // iterates the borrowed doc_reference.
                doc.rewind();
                yield_parser.set_borrowed_document(
                    simdjson::ondemand::document_reference(doc));
                co_yield JsonLine{src, 0, &yield_parser};
            }
        }
        co_return;
    }

    // Fallback: non-indexed paths use the per-line pipeline unchanged.
    config.chunk_prune_only = true;
    auto line_gen = read_lines(config);

    common::json::JsonParser parser;

    while (auto opt = co_await line_gen.next()) {
        const char* trimmed;
        std::size_t trimmed_len;
        if (!dftracer::utils::json_trim_and_validate_with_comma(
                opt->content.data(), opt->content.size(), trimmed, trimmed_len))
            continue;
        if (!parser.parse(std::string_view(trimmed, trimmed_len))) continue;

        if (query) {
            common::query::ValueMap fields;
            std::vector<std::string> nested_keys;
            parser.for_each_field(
                [&](std::string_view key, simdjson::ondemand::value val) {
                    auto type = val.type().value_unsafe();
                    if (type == simdjson::ondemand::json_type::object) {
                        nested_keys.emplace_back(key);
                    } else if (query->references(key)) {
                        fields[std::string(key)] = ondemand_to_literal(val);
                    }
                });
            for (auto& nk : nested_keys) {
                parser.rewind();
                parser.for_each_field(nk, [&](std::string_view key,
                                              simdjson::ondemand::value val) {
                    if (query->references(key)) {
                        fields[std::string(key)] = ondemand_to_literal(val);
                    }
                });
            }
            if (!query->evaluate(fields)) continue;
            parser.rewind();
        }

        co_yield JsonLine{opt->content, opt->line_number, &parser};
    }
}

coro::AsyncGenerator<std::span<const char>> TraceReader::read_raw(
    ReadConfig config) {
    if (has_index_) {
        // Keep RocksDB alive for the generator's lifetime so per-method
        // opens in GzipIndexer reuse DBManager's cached handle.
        std::optional<indexer::IndexDatabase> db_keep_alive;
        if (!index_path_.empty()) {
            try {
                db_keep_alive.emplace(
                    index_path_, rocksdb::RocksDatabase::OpenMode::ReadOnly);
            } catch (...) {
            }
        }
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

        if (!config.query.empty() && !index_path_.empty() &&
            range_type == internal::RangeType::BYTE_RANGE) {
            auto parsed = Query::from_string(config.query);
            if (!parsed) throw common::query::QueryParseError(parsed.error());
            ChunkPrunerInput pruner_input{index_path_, config_.file_path,
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
