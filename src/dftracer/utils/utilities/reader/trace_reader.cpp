// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/archive_format.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_arena.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/plan/prefilter.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/json/json_value.h>
#include <dftracer/utils/json/line.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/time_metric_event.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/reader/error.h>
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
#include <type_traits>

namespace dftracer::utils::utilities::reader {

using dftracer::utils::index::gzip::CheckpointIndexerFactory;
using duql::Query;
using json::JsonValue;

using internal::read_chunks_indexed;
using internal::strip_ndjson_bookends;

namespace {

thread_local dftracer::utils::json::RecordParser tl_parser;

/// Traces must be gzip. A bare .pfw is rejected rather than read, so a file
/// that was never compressed fails at the reader instead of silently taking
/// a path with no index, no pruning and no random access.
void reject_unless_gzip(common::ArchiveFormat format,
                        const dstd::String& file_path) {
    if (format == common::ArchiveFormat::Gzip) return;
    throw ReaderError(ReaderError::InvalidArgument,
                      "Not a gzip trace: " + file_path +
                          " (dftracer traces must be gzip-compressed)");
}

bool line_matches_query(const Query& q, dstd::StringView content) {
    auto result = tl_parser.parse(content.data(), content.size());
    if (result.error()) return false;
    auto root = result.value_unsafe();
    if (!root.is_object()) return false;
    JsonValue json(root);
    return duql::evaluate(duql::detail::QueryAccess::root(q), json);
}

struct LineRange {
    std::size_t start_line;
    std::size_t end_line;
};

coro::AsyncGenerator<Line> yield_lines_from_stream(
    dstd::UniquePtr<internal::ReaderStream> stream, std::size_t start_line_num,
    const Query* query, bool chunk_prune_only = false,
    const index::plan::Prefilter* prefilter = nullptr) {
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
            !prefilter->may_match(dstd::StringView(data, len))) {
            line_num += std::count(data, data + len, '\n');
            continue;
        }

        std::size_t pos = 0;
        while (pos < len) {
            const void* nl_ptr = std::memchr(data + pos, '\n', len - pos);
            std::size_t end_pos =
                nl_ptr ? static_cast<const char*>(nl_ptr) - data : len;
            if (end_pos > pos) {
                auto line_sv = dstd::StringView(data + pos, end_pos - pos);
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
    dstd::SharedPtr<internal::Reader> reader, dstd::Vector<LineRange> ranges,
    std::size_t buffer_size, Query query, bool chunk_prune_only = false,
    index::plan::Prefilter prefilter = {}) {
    for (const auto& range : ranges) {
        auto stream =
            reader->stream(internal::StreamConfig()
                               .stream_type(internal::StreamType::MultiLines)
                               .range_type(internal::RangeType::LineRange)
                               .from(range.start_line)
                               .to(range.end_line)
                               .buffer_size(buffer_size));
        auto gen =
            yield_lines_from_stream(std::move(stream), range.start_line, &query,
                                    chunk_prune_only, &prefilter);
        for (;;) {
            auto line = co_await gen.next();
            if (!line) break;
            co_yield *line;
        }
    }
}

// Raw-chunk variants of the yield/read helpers. Same pruning logic as the
// line-yielding flavors but emit dstd::Span<const char> buffers untouched
// (multi-line boundary respected by stream type). Used by read_json to run
// parse each line of a chunk in place instead of reading line by line.
coro::AsyncGenerator<dstd::Span<const char>> yield_chunks_from_stream(
    dstd::UniquePtr<internal::ReaderStream> stream,
    const index::plan::Prefilter* prefilter = nullptr) {
    while (!stream->done()) {
        auto chunk = co_await stream->read_async();
        if (chunk.empty()) break;
        if (prefilter && !prefilter->empty() &&
            !prefilter->may_match(
                dstd::StringView(chunk.data(), chunk.size()))) {
            continue;
        }
        co_yield chunk;
    }
}

coro::AsyncGenerator<dstd::Span<const char>> yield_chunks_from_ranges(
    dstd::SharedPtr<internal::Reader> reader, dstd::Vector<LineRange> ranges,
    std::size_t buffer_size, index::plan::Prefilter prefilter = {}) {
    for (const auto& range : ranges) {
        auto stream =
            reader->stream(internal::StreamConfig()
                               .stream_type(internal::StreamType::MultiLines)
                               .range_type(internal::RangeType::LineRange)
                               .from(range.start_line)
                               .to(range.end_line)
                               .buffer_size(buffer_size));
        auto gen = yield_chunks_from_stream(std::move(stream), &prefilter);
        for (;;) {
            auto chunk = co_await gen.next();
            if (!chunk) break;
            co_yield *chunk;
        }
    }
}

coro::AsyncGenerator<Line> read_lines_indexed(
    dstd::SharedPtr<internal::Reader> reader, dstd::String index_path,
    dstd::String file_path, ReadConfig config, dstd::Optional<Query> query,
    bool chunk_prune_only = false) {
    index::plan::Prefilter prefilter =
        query ? index::plan::Prefilter(*query) : index::plan::Prefilter{};
    auto range_type = config.has_line_range() ? internal::RangeType::LineRange
                                              : internal::RangeType::ByteRange;
    std::size_t start =
        config.has_line_range() ? config.start_line : config.start_byte;
    std::size_t end =
        config.has_line_range() ? config.end_line : config.end_byte;

    if (range_type == internal::RangeType::LineRange) {
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
        range_type == internal::RangeType::ByteRange) {
        auto snapshot = common::unwrap(
            dftracer::utils::index::plan::open_snapshot(index_path));
        // GCC 12 destroys a temporary built inside co_await twice.
        dftracer::utils::index::plan::PruneRequest prune_req{
            .index_path = index_path,
            .file_path = file_path,
            .query = &*query,
            .snapshot = &snapshot,
            .metadata = dftracer::utils::index::plan::MetadataUse::All};
        auto pruned = co_await dftracer::utils::index::plan::prune_file(
            std::move(prune_req));
        if (!pruned->file_may_match) co_return;

        if (!pruned->all_chunks &&
            pruned->candidates.size() < pruned->total_chunks) {
            auto logical =
                dftracer::utils::index::store::internal::get_logical_path(
                    file_path);
            const auto file = snapshot.file(logical);

            if (file) {
                auto spans = common::unwrap(
                    dftracer::utils::index::spans(snapshot, *file));

                dstd::Vector<LineRange> ranges;
                std::uint64_t prev_idx = UINT64_MAX;

                for (auto ckpt_idx : pruned->candidates) {
                    if (ckpt_idx >= spans.size()) continue;
                    const auto& ckpt = spans[ckpt_idx];

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
                for (;;) {
                    auto line = co_await gen.next();
                    if (!line) break;
                    co_yield *line;
                }
                co_return;
            }
        }
    }

    auto stream =
        reader->stream(internal::StreamConfig()
                           .stream_type(internal::StreamType::MultiLines)
                           .range_type(range_type)
                           .from(start)
                           .to(end)
                           .buffer_size(config.buffer_size));

    auto gen = yield_lines_from_stream(std::move(stream), start,
                                       query ? &*query : nullptr,
                                       chunk_prune_only, &prefilter);
    for (;;) {
        auto line = co_await gen.next();
        if (!line) break;
        co_yield *line;
    }
}

coro::AsyncGenerator<Line> read_lines_gz(dstd::String file_path,
                                         ReadConfig config,
                                         dstd::Optional<Query> query,
                                         bool chunk_prune_only = false) {
    std::size_t start = config.has_line_range() ? config.start_line : 0;
    std::size_t end = config.has_line_range() ? config.end_line : 0;
    const bool byte_range = config.has_byte_range();
    // Offsets are into the uncompressed stream, as everywhere else. A line
    // starting before the range is a partial and is dropped; one starting
    // inside it is emitted whole, so the range completes its last line.
    std::size_t byte_pos = 0;

    auto gen =
        fileio::lines::sources::async_streaming_gz_lines(file_path, start, end);
    for (;;) {
        auto opt = co_await gen.next();
        if (!opt) break;
        const std::size_t line_start = byte_pos;
        byte_pos += opt->content.size() + 1;

        if (byte_range) {
            if (line_start < config.start_byte) continue;
            if (config.end_byte > 0 && line_start >= config.end_byte) break;
        }
        if (chunk_prune_only || !query ||
            line_matches_query(*query, opt->content)) {
            co_yield *opt;
        }
    }
}

}  // namespace

namespace internal {

coro::AsyncGenerator<dstd::Span<const char>> read_chunks_indexed(
    dstd::SharedPtr<internal::Reader> reader, dstd::String index_path,
    dstd::String file_path, ReadConfig config, dstd::Optional<Query> query,
    bool extend_to_line_boundary) {
    index::plan::Prefilter prefilter =
        query ? index::plan::Prefilter(*query) : index::plan::Prefilter{};
    auto range_type = config.has_line_range() ? internal::RangeType::LineRange
                                              : internal::RangeType::ByteRange;
    std::size_t start =
        config.has_line_range() ? config.start_line : config.start_byte;
    std::size_t end =
        config.has_line_range() ? config.end_line : config.end_byte;

    if (range_type == internal::RangeType::LineRange) {
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
        auto snapshot = common::unwrap(
            dftracer::utils::index::plan::open_snapshot(index_path));
        // GCC 12 destroys a temporary built inside co_await twice.
        dftracer::utils::index::plan::PruneRequest prune_req{
            .index_path = index_path,
            .file_path = file_path,
            .query = &*query,
            .snapshot = &snapshot,
            .metadata = dftracer::utils::index::plan::MetadataUse::All};
        auto pruned = co_await dftracer::utils::index::plan::prune_file(
            std::move(prune_req));
        if (!pruned->file_may_match) co_return;

        if (!pruned->all_chunks &&
            pruned->candidates.size() < pruned->total_chunks) {
            auto logical =
                dftracer::utils::index::store::internal::get_logical_path(
                    file_path);
            const auto file = snapshot.file(logical);

            if (file) {
                auto spans = common::unwrap(
                    dftracer::utils::index::spans(snapshot, *file));

                dstd::Vector<LineRange> ranges;
                std::uint64_t prev_idx = UINT64_MAX;
                for (auto ckpt_idx : pruned->candidates) {
                    if (ckpt_idx >= spans.size()) continue;
                    const auto& ckpt = spans[ckpt_idx];
                    // Intersect with the caller's window (byte or line) so
                    // chunk-level parallel work items stay disjoint.
                    if (range_type == internal::RangeType::ByteRange) {
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
                for (;;) {
                    auto chunk = co_await gen.next();
                    if (!chunk) break;
                    co_yield *chunk;
                }
                co_return;
            }
        }
    }

    auto stream_type = (range_type == internal::RangeType::ByteRange)
                           ? internal::StreamType::MultiLinesBytes
                           : internal::StreamType::MultiLines;
    auto stream =
        reader->stream(internal::StreamConfig()
                           .stream_type(stream_type)
                           .range_type(range_type)
                           .from(start)
                           .to(end)
                           .buffer_size(config.buffer_size)
                           .extend_to_line_boundary(
                               extend_to_line_boundary &&
                               range_type == internal::RangeType::ByteRange));

    auto gen = yield_chunks_from_stream(std::move(stream), &prefilter);
    for (;;) {
        auto chunk = co_await gen.next();
        if (!chunk) break;
        co_yield *chunk;
    }
}

}  // namespace internal

TraceReader::TraceReader(TraceReaderConfig config)
    : config_(std::move(config)) {
    probe_index();
}

void TraceReader::probe_index() {
    format_ = CheckpointIndexerFactory::detect_format(config_.file_path);
    index_path_ = trace::internal::determine_index_path(config_.file_path,
                                                        config_.index_dir);
    has_index_ = format_ == common::ArchiveFormat::Gzip &&
                 common::unwrap(dstd::fs::exists(dstd::PathView(
                     dftracer::utils::index::store_dir(index_path_))));
    // Do not trust an index whose source changed since it was built; fall back
    // to a raw read rather than serving stale data. Uses the shared stat-based
    // freshness predicate, so a record with no stored stat is treated as stale
    // rather than trusted on existence alone.
    if (has_index_) {
        auto snapshot =
            dftracer::utils::index::plan::open_snapshot(index_path_);
        if (snapshot) {
            dftracer::utils::common::InputFileStat st;
            dftracer::utils::index::SourceStat now;
            if (dftracer::utils::common::stat_input(config_.file_path, st)) {
                now.mtime = static_cast<std::uint64_t>(st.mtime);
                now.size = st.size;
            }
            if (snapshot->freshness(
                    dftracer::utils::index::store::internal::get_logical_path(
                        config_.file_path),
                    now) != dftracer::utils::index::Freshness::Current) {
                has_index_ = false;
            }
        }
    }
}

bool TraceReader::has_index() const { return has_index_; }

void TraceReader::ensure_metadata_cached() {
    if (metadata_cached_) return;

    if (has_index_) {
        auto reader = create_indexed_reader();
        cached_max_bytes_ = reader->get_max_bytes();
        cached_num_lines_ = reader->get_num_lines();
    } else {
        cached_max_bytes_ = 0;
        cached_num_lines_ = 0;
    }
    metadata_cached_ = true;
}

coro::CoroTask<trace::TimeMetric> TraceReader::read_time_metric(
    std::size_t max_lines) {
    using trace::DFTracerEvent;
    using trace::TimeMetric;

    ReadConfig probe;
    probe.end_line = max_lines;
    auto gen = read_lines(probe);
    dftracer::utils::json::RecordParser parser;
    TimeMetric metric = TimeMetric::Us;
    for (;;) {
        auto line_opt = co_await gen.next();
        if (!line_opt) break;
        const char* start = nullptr;
        std::size_t len = 0;
        if (!json::trim_and_validate(line_opt->content.data(),
                                     line_opt->content.size(), start, len))
            continue;
        auto doc = parser.parse(start, len);
        if (doc.error()) continue;
        DFTracerEvent event;
        if (!DFTracerEvent::parse(json::JsonValue(doc.value()), event))
            continue;
        if (trace::extract_time_metric(event, metric)) break;
        // CM precedes timeline events; stop once past the metadata header.
        if (event.is_event()) break;
    }
    co_return metric;
}

std::size_t TraceReader::get_max_bytes() {
    ensure_metadata_cached();
    return cached_max_bytes_;
}

std::size_t TraceReader::get_num_lines() {
    ensure_metadata_cached();
    return cached_num_lines_;
}

dstd::SharedPtr<internal::Reader> TraceReader::create_indexed_reader() {
    auto indexer = CheckpointIndexerFactory::create(
        config_.file_path, index_path_, config_.checkpoint_size, false);
    return internal::ReaderFactory::create(indexer);
}

internal::StreamType TraceReader::resolve_raw_stream_type(
    const ReadConfig& config) const {
    if (!config.line_aligned) return internal::StreamType::Bytes;
    if (config.multi_line) return internal::StreamType::MultiLinesBytes;
    return internal::StreamType::LineBytes;
}

internal::RangeType TraceReader::resolve_range_type(
    const ReadConfig& config) const {
    if (config.has_line_range()) return internal::RangeType::LineRange;
    return internal::RangeType::ByteRange;
}

coro::AsyncGenerator<Line> TraceReader::read_lines(ReadConfig config) {
    dstd::Optional<Query> query;
    if (!config.query.empty()) {
        auto parsed = Query::from_string(config.query);
        if (!parsed) throw duql::DuqlParseError(parsed.error());
        query = std::move(*parsed);
    }

    bool cpo = config.chunk_prune_only;

    if (has_index_) {
        return read_lines_indexed(create_indexed_reader(), index_path_,
                                  config_.file_path, std::move(config),
                                  std::move(query), cpo);
    }
    reject_unless_gzip(format_, config_.file_path);
    return read_lines_gz(config_.file_path, std::move(config), std::move(query),
                         cpo);
}

coro::AsyncGenerator<JsonLine> TraceReader::read_json(ReadConfig config) {
    dstd::Optional<Query> query;
    if (!config.query.empty()) {
        auto parsed = Query::from_string(config.query);
        if (!parsed) throw duql::DuqlParseError(parsed.error());
        query = std::move(*parsed);
    }

    // chunk_prune_only path: dim_stats already proved every event with the
    // predicate field matches; we still need to skip events lacking the
    // field (e.g., metadata "ph":"M" events). Field-presence probe is
    // cheaper than full ValueMap eval.
    dstd::Vector<dstd::String> presence_check_paths;
    if (query && config.chunk_prune_only) {
        const auto& fset = query->fields();
        presence_check_paths.assign(fset.begin(), fset.end());
    }

    // Whether any nested field is referenced by dotted path (e.g. "args.ret"),
    // in which case the ValueMap builders store the dotted key too.
    const bool query_has_dotted =
        query && internal::query_references_dotted(*query);

    // Fast path: indexed gz files go through a chunk generator with
    // per-line in-place parsing. Query is evaluated on the ondemand document
    // directly, so non-matching docs never hit the yield_parser.
    if (has_index_) {
        auto reader = create_indexed_reader();
        auto chunk_gen = read_chunks_indexed(reader, index_path_,
                                             config_.file_path, config, query);

        simdjson::ondemand::parser bulk_parser;
        json::JsonParser yield_parser;
        // An expression may read a whole object or array, which the field map
        // cannot hold, so it runs on the record's DOM.
        dstd::StringArena event_strings;

        for (;;) {
            auto chunk_opt = co_await chunk_gen.next();
            if (!chunk_opt) break;
            auto chunk = *chunk_opt;
            if (chunk.empty()) continue;
            auto trimmed = strip_ndjson_bookends(
                dstd::StringView(chunk.data(), chunk.size()));
            if (trimmed.empty()) continue;
            simdjson::padded_string padded(trimmed);

            internal::PaddedLines docs(bulk_parser, padded.data(),
                                       padded.size());
            simdjson::ondemand::document_reference doc;
            dstd::StringView src;
            while (docs.next(doc, src)) {
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
                } else if (query && query->has_expressions()) {
                    event_strings.clear();
                    if (!duql::evaluate(
                            duql::detail::QueryAccess::root(*query),
                            duql::OnDemandEvent(doc, src, event_strings)))
                        continue;
                } else if (query) {
                    duql::ValueMap fields;
                    // A syntax error stops the read and drops the line.
                    bool readable = true;
                    auto obj = doc.get_object();
                    if (obj.error()) continue;
                    for (auto field : obj.value()) {
                        if (field.error()) {
                            readable = false;
                            break;
                        }
                        const auto key_r = json::detail::get_key_or_raw(field);
                        if (!key_r) {
                            readable = false;
                            break;
                        }
                        auto val_r = field.value();
                        if (val_r.error()) {
                            readable = false;
                            break;
                        }
                        auto key = *key_r;
                        auto val = val_r.value();
                        auto type_r = val.type();
                        if (type_r.error()) {
                            readable = false;
                            break;
                        }
                        auto type = type_r.value();
                        if (type == simdjson::ondemand::json_type::object) {
                            auto nested = val.get_object();
                            if (nested.error()) {
                                readable = false;
                                break;
                            }
                            for (auto nf : nested.value()) {
                                if (nf.error()) {
                                    readable = false;
                                    break;
                                }
                                const auto nk_r =
                                    json::detail::get_key_or_raw(nf);
                                auto nv_r = nf.value();
                                if (!nk_r || nv_r.error()) {
                                    readable = false;
                                    break;
                                }
                                if (!internal::store_referenced_nested(
                                        fields, *query, query_has_dotted, key,
                                        *nk_r, nv_r.value())) {
                                    readable = false;
                                    break;
                                }
                            }
                            if (!readable) break;
                        } else {
                            if (!internal::store_referenced(fields, *query, key,
                                                            val, type)) {
                                readable = false;
                                break;
                            }
                        }
                    }
                    if (!readable || !query->evaluate(fields)) continue;
                }

                // Matched (or no query): lend the line's document to
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

    json::JsonParser parser;
    dstd::StringArena event_strings;

    for (;;) {
        auto opt = co_await line_gen.next();
        if (!opt) break;
        const char* trimmed;
        std::size_t trimmed_len;
        if (!dftracer::utils::json::trim_and_validate_with_comma(
                opt->content.data(), opt->content.size(), trimmed, trimmed_len))
            continue;
        if (!parser.parse(dstd::StringView(trimmed, trimmed_len))) continue;

        if (query && query->has_expressions()) {
            event_strings.clear();
            if (!duql::evaluate(
                    duql::detail::QueryAccess::root(*query),
                    duql::OnDemandEvent(parser.raw_document(), parser.text(),
                                        event_strings)))
                continue;
            parser.rewind();
        } else if (query) {
            duql::ValueMap fields;
            dstd::Vector<dstd::String> nested_keys;
            bool readable = true;
            parser.for_each_field(
                [&](dstd::StringView key, simdjson::ondemand::value val) {
                    if (!readable) return;
                    const auto type_r = val.type();
                    if (type_r.error()) {
                        readable = false;
                        return;
                    }
                    const auto type = type_r.value_unsafe();
                    if (type == simdjson::ondemand::json_type::object) {
                        nested_keys.emplace_back(key);
                    } else if (!internal::store_referenced(fields, *query, key,
                                                           val, type)) {
                        readable = false;
                    }
                });
            for (auto& nk : nested_keys) {
                if (!readable) break;
                parser.rewind();
                parser.for_each_field(nk, [&](dstd::StringView key,
                                              simdjson::ondemand::value val) {
                    if (readable &&
                        !internal::store_referenced_nested(
                            fields, *query, query_has_dotted, nk, key, val))
                        readable = false;
                });
            }
            if (!readable || !parser.raw_document().is_alive() ||
                !query->evaluate(fields))
                continue;
            parser.rewind();
        }

        co_yield JsonLine{opt->content, opt->line_number, &parser};
    }
}

coro::AsyncGenerator<dstd::Span<const char>> TraceReader::read_raw(
    ReadConfig config) {
    if (has_index_) {
        auto reader = create_indexed_reader();
        auto stream_type = resolve_raw_stream_type(config);
        auto range_type = resolve_range_type(config);
        std::size_t start =
            config.has_line_range() ? config.start_line : config.start_byte;
        std::size_t end =
            config.has_line_range() ? config.end_line : config.end_byte;

        if (range_type == internal::RangeType::LineRange) {
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
            range_type == internal::RangeType::ByteRange) {
            auto parsed = Query::from_string(config.query);
            if (!parsed) throw duql::DuqlParseError(parsed.error());
            auto snapshot = common::unwrap(
                dftracer::utils::index::plan::open_snapshot(index_path_));
            // GCC 12 destroys a temporary built inside co_await twice.
            dftracer::utils::index::plan::PruneRequest prune_req{
                .index_path = index_path_,
                .file_path = config_.file_path,
                .query = &*parsed,
                .snapshot = &snapshot,
                .metadata = dftracer::utils::index::plan::MetadataUse::All};
            auto pruned = co_await dftracer::utils::index::plan::prune_file(
                std::move(prune_req));
            if (!pruned->file_may_match) co_return;
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
    } else if (format_ == common::ArchiveFormat::Gzip) {
        auto gen =
            fileio::lines::sources::async_streaming_gz_lines(config_.file_path);
        std::size_t byte_pos = 0;
        for (;;) {
            auto opt = co_await gen.next();
            if (!opt) break;
            const auto& line = *opt;
            std::size_t line_end = byte_pos + line.content.size() + 1;
            if (config.end_byte > 0 && byte_pos >= config.end_byte) break;
            if (line_end > config.start_byte) {
                co_yield dstd::Span<const char>(line.content.data(),
                                                line.content.size());
            }
            byte_pos = line_end;
        }
    }
}

}  // namespace dftracer::utils::utilities::reader
