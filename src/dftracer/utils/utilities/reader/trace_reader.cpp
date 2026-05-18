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
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <simdjson.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#endif

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

// Cheap byte-level pre-filter derived from a query AST.
//
// The filter holds a list of literal substrings that MUST appear (verbatim) in
// any line matching the query. Currently populated only for ASTs of the form
// "AND of field == literal"; the common shape of dftindex equality queries.
// For unsupported shapes (range ops, OR, NOT, IN/NOT IN, non-equality compares)
// `required` is left empty and `may_match` trivially returns true.
//
// Semantically false-positive-safe: any line we accept still gets re-checked
// against the real query downstream. Lines we reject are guaranteed not to
// match because the literal representation of the comparison is missing.
struct LinePrefilter {
    std::vector<std::string> required;

    bool empty() const { return required.empty(); }

    bool may_match(std::string_view bytes) const {
        for (const auto& lit : required) {
            if (::memmem(bytes.data(), bytes.size(), lit.data(), lit.size()) ==
                nullptr)
                return false;
        }
        return true;
    }
};

bool collect_and_eq_literals(const common::query::QueryNode& node,
                             std::vector<std::string>& out) {
    return std::visit(
        [&out](const auto& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, common::query::CompareNode>) {
                if (n.op != common::query::CompareOp::EQ) return false;
                std::string lit;
                lit.reserve(n.field.path.size() + 16);
                lit += '"';
                lit += n.field.path;
                lit += "\":";
                const auto& val = n.value.value;
                if (std::holds_alternative<std::string>(val)) {
                    lit += '"';
                    lit += std::get<std::string>(val);
                    lit += '"';
                } else if (std::holds_alternative<int64_t>(val)) {
                    lit += std::to_string(std::get<int64_t>(val));
                } else if (std::holds_alternative<uint64_t>(val)) {
                    lit += std::to_string(std::get<uint64_t>(val));
                } else if (std::holds_alternative<bool>(val)) {
                    lit += std::get<bool>(val) ? "true" : "false";
                } else {
                    return false;  // double or other: skip pre-filter
                }
                out.push_back(std::move(lit));
                return true;
            } else if constexpr (std::is_same_v<T, common::query::AndNode>) {
                return collect_and_eq_literals(*n.left, out) &&
                       collect_and_eq_literals(*n.right, out);
            }
            return false;  // OrNode, NotNode, InNode, NotInNode, CompareNode
                           // with non-EQ op: conservative skip
        },
        node.data);
}

// Strip a leading `[` and trailing `]` (plus surrounding whitespace) from a
// chunk buffer. These bookends appear in `.pfw.gz` files to keep them
// Perfetto-viewable as JSON arrays, but break simdjson iterate_many which
// expects whitespace-separated NDJSON. Safe to call on any chunk: if the
// bookends are absent the range is returned unchanged.
std::string_view strip_ndjson_bookends(std::string_view bytes) {
    const char* s = bytes.data();
    const char* e = bytes.data() + bytes.size();
    auto is_ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    while (s < e && is_ws(*s)) ++s;
    if (s < e && *s == '[') {
        ++s;
        while (s < e && is_ws(*s)) ++s;
    }
    while (e > s && is_ws(e[-1])) --e;
    if (e > s && e[-1] == ']') {
        --e;
        while (e > s && is_ws(e[-1])) --e;
    }
    return std::string_view(s, static_cast<std::size_t>(e - s));
}

// AND-of-EQ predicates with concrete typed literals can be evaluated
// directly against simdjson without going through ValueMap (which costs
// wyhash + per-field std::string allocation per row). Anything more
// complex (OR/NOT/IN/range) falls back to the generic visitor.
struct CompiledEqProbe {
    std::string top_key;     // "pid", "args", "name", etc.
    std::string nested_key;  // "" for top-level, else e.g. "fhash"
    enum class Kind { String, Int64, UInt64, Double, Bool };
    Kind kind = Kind::String;
    std::string s_val;
    std::int64_t i64_val = 0;
    std::uint64_t u64_val = 0;
    double d_val = 0.0;
    bool b_val = false;
};

// Top-level JSON keys in dftracer events. Anything else in the query DSL
// (e.g. `epoch == 0`, `fhash == "..."`) refers to a field nested under
// "args"; the same convention collect_query_fields relies on when it
// folds nested object keys into the flat ValueMap.
bool is_top_level_event_key(std::string_view k) {
    return k == "id" || k == "name" || k == "cat" || k == "pid" || k == "tid" ||
           k == "ts" || k == "dur" || k == "ph";
}

// Walk a CompareNode-with-EQ leaf into a probe. Returns false on
// unsupported shapes (more than one '.' or a literal type the simdjson
// get_X path can't compare directly).
bool compile_eq_leaf(const common::query::CompareNode& n,
                     CompiledEqProbe& out) {
    if (n.op != common::query::CompareOp::EQ) return false;
    auto dot = n.field.path.find('.');
    if (dot == std::string::npos) {
        if (is_top_level_event_key(n.field.path)) {
            out.top_key = n.field.path;
            out.nested_key.clear();
        } else {
            // Bare arg-style key: foo -> args.foo.
            out.top_key = "args";
            out.nested_key = n.field.path;
        }
    } else {
        if (n.field.path.find('.', dot + 1) != std::string::npos) return false;
        out.top_key = n.field.path.substr(0, dot);
        out.nested_key = n.field.path.substr(dot + 1);
    }
    return std::visit(
        [&out](auto&& v) -> bool {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                out.kind = CompiledEqProbe::Kind::String;
                out.s_val = v;
                return true;
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                out.kind = CompiledEqProbe::Kind::Int64;
                out.i64_val = v;
                return true;
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                out.kind = CompiledEqProbe::Kind::UInt64;
                out.u64_val = v;
                return true;
            } else if constexpr (std::is_same_v<T, double>) {
                out.kind = CompiledEqProbe::Kind::Double;
                out.d_val = v;
                return true;
            } else if constexpr (std::is_same_v<T, bool>) {
                out.kind = CompiledEqProbe::Kind::Bool;
                out.b_val = v;
                return true;
            } else {
                return false;
            }
        },
        n.value.value);
}

// Try to compile the query AST as an AND of EQ leaves. nullopt on
// unsupported shapes; the ValueMap path handles those.
std::optional<std::vector<CompiledEqProbe>> try_compile_eq_probes(
    const common::query::QueryNode& node) {
    using namespace common::query;
    return std::visit(
        [&](const auto& n) -> std::optional<std::vector<CompiledEqProbe>> {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, CompareNode>) {
                CompiledEqProbe p;
                if (!compile_eq_leaf(n, p)) return std::nullopt;
                return std::vector<CompiledEqProbe>{std::move(p)};
            } else if constexpr (std::is_same_v<T, AndNode>) {
                auto l = try_compile_eq_probes(*n.left);
                if (!l) return std::nullopt;
                auto r = try_compile_eq_probes(*n.right);
                if (!r) return std::nullopt;
                l->insert(l->end(), std::make_move_iterator(r->begin()),
                          std::make_move_iterator(r->end()));
                return l;
            } else {
                return std::nullopt;
            }
        },
        node.data);
}

bool probe_matches_value(const CompiledEqProbe& p,
                         simdjson::ondemand::value val) {
    switch (p.kind) {
        case CompiledEqProbe::Kind::String: {
            auto r = val.get_string();
            if (r.error()) return false;
            auto sv = r.value_unsafe();
            return sv.size() == p.s_val.size() &&
                   std::memcmp(sv.data(), p.s_val.data(), sv.size()) == 0;
        }
        case CompiledEqProbe::Kind::Int64: {
            auto t = val.type();
            if (t.error()) return false;
            if (t.value_unsafe() == simdjson::ondemand::json_type::number) {
                auto num = val.get_number();
                if (num.error()) return false;
                auto n = num.value_unsafe();
                if (n.is_int64()) return n.get_int64() == p.i64_val;
                if (n.is_uint64()) {
                    if (p.i64_val < 0) return false;
                    return n.get_uint64() ==
                           static_cast<std::uint64_t>(p.i64_val);
                }
                return n.get_double() == static_cast<double>(p.i64_val);
            }
            return false;
        }
        case CompiledEqProbe::Kind::UInt64: {
            auto num = val.get_number();
            if (num.error()) return false;
            auto n = num.value_unsafe();
            if (n.is_uint64()) return n.get_uint64() == p.u64_val;
            if (n.is_int64()) {
                auto v = n.get_int64();
                if (v < 0) return false;
                return static_cast<std::uint64_t>(v) == p.u64_val;
            }
            return n.get_double() == static_cast<double>(p.u64_val);
        }
        case CompiledEqProbe::Kind::Double: {
            auto r = val.get_double();
            if (r.error()) return false;
            return r.value_unsafe() == p.d_val;
        }
        case CompiledEqProbe::Kind::Bool: {
            auto r = val.get_bool();
            if (r.error()) return false;
            return r.value_unsafe() == p.b_val;
        }
    }
    return false;
}

// Evaluate compiled AND-of-EQ probes by directly probing simdjson fields.
bool eval_compiled_eq(const std::vector<CompiledEqProbe>& probes,
                      simdjson::ondemand::document_reference doc) {
    for (const auto& p : probes) {
        doc.rewind();
        auto top_r = doc.find_field_unordered(
            std::string_view(p.top_key.data(), p.top_key.size()));
        if (top_r.error()) return false;
        auto top_v = top_r.value();
        if (p.nested_key.empty()) {
            if (!probe_matches_value(p, top_v)) return false;
        } else {
            auto obj_r = top_v.get_object();
            if (obj_r.error()) return false;
            auto inner_r = obj_r.value().find_field_unordered(
                std::string_view(p.nested_key.data(), p.nested_key.size()));
            if (inner_r.error()) return false;
            if (!probe_matches_value(p, inner_r.value())) return false;
        }
    }
    return true;
}

LinePrefilter build_prefilter(const Query& q) {
    // Short literals like `"pid":1000` or `"epoch":0` are common enough in
    // practice that memmem on every line costs more than it saves on the
    // parse side. Only keep literals long enough that rarity is plausible
    // (hashes, filenames, host names).
    constexpr std::size_t MIN_LITERAL_LEN = 16;

    LinePrefilter pf;
    std::vector<std::string> tmp;
    if (collect_and_eq_literals(q.root(), tmp)) {
        for (auto& lit : tmp) {
            if (lit.size() >= MIN_LITERAL_LEN) {
                pf.required.push_back(std::move(lit));
            }
        }
    }
    return pf;
}

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

coro::AsyncGenerator<std::span<const char>> read_chunks_indexed(
    std::shared_ptr<internal::Reader> reader, std::string index_path,
    std::string file_path, ReadConfig config, std::optional<Query> query,
    bool extend_to_line_boundary = false) {
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

namespace {

common::query::LiteralValue ondemand_to_literal(simdjson::ondemand::value val) {
    auto type = val.type().value_unsafe();
    switch (type) {
        case simdjson::ondemand::json_type::string: {
            auto r = val.get_string();
            if (!r.error()) return std::string(r.value_unsafe());
            break;
        }
        case simdjson::ondemand::json_type::number: {
            auto num = val.get_number();
            if (!num.error()) {
                auto n = num.value_unsafe();
                if (n.is_int64()) return n.get_int64();
                if (n.is_uint64()) return n.get_uint64();
                return n.get_double();
            }
            break;
        }
        case simdjson::ondemand::json_type::boolean: {
            auto r = val.get_bool();
            if (!r.error()) return r.value_unsafe();
            break;
        }
        default:
            break;
    }
    return std::string{};
}

}  // namespace

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

#ifdef DFTRACER_UTILS_ENABLE_ARROW

namespace {

using common::arrow::ArrowExportResult;
using common::arrow::ColumnType;
using common::arrow::RecordBatchBuilder;

// Bump arena for string_views that must survive until builder.finish().
struct ArrowStringArena {
    static constexpr std::size_t BLOCK_SIZE = 64 * 1024;
    std::vector<std::vector<char>> blocks;
    std::size_t pos = 0;

    ArrowStringArena() { blocks.emplace_back(BLOCK_SIZE); }

    std::string_view push(const char* data, std::size_t len) {
        if (pos + len > blocks.back().size()) {
            blocks.emplace_back(std::max(BLOCK_SIZE, len));
            pos = 0;
        }
        char* dst = blocks.back().data() + pos;
        std::memcpy(dst, data, len);
        pos += len;
        return {dst, len};
    }

    void clear() {
        if (blocks.size() > 1) blocks.resize(1);
        pos = 0;
    }
};

struct ArrowKeyHint {
    std::string key;
    std::size_t col_idx = 0;
    ColumnType type = ColumnType::INT64;
    bool valid = false;
};

inline std::size_t resolve_col_idx(RecordBatchBuilder& builder,
                                   std::vector<ArrowKeyHint>& hints,
                                   std::size_t pos, std::string_view key_sv,
                                   ColumnType type) {
    if (pos < hints.size()) {
        auto& h = hints[pos];
        if (h.valid && h.type == type && h.key.size() == key_sv.size() &&
            std::memcmp(h.key.data(), key_sv.data(), key_sv.size()) == 0) {
            return h.col_idx;
        }
    }
    // Position-keyed miss. Variable-shape rows (e.g., open vs read events
    // with different args fields) push fields to different positions, so
    // the position cache misses constantly while the underlying schema is
    // small (~15 keys). A linear scan over the hint vector with a SIMD
    // memcmp beats RecordBatchBuilder's name_to_index_ hash lookup for this
    // size.
    for (std::size_t i = 0; i < hints.size(); ++i) {
        if (i == pos) continue;
        auto& h = hints[i];
        if (h.valid && h.type == type && h.key.size() == key_sv.size() &&
            std::memcmp(h.key.data(), key_sv.data(), key_sv.size()) == 0) {
            if (pos < hints.size()) {
                auto& slot = hints[pos];
                slot.key.assign(key_sv);
                slot.type = type;
                slot.col_idx = h.col_idx;
                slot.valid = true;
            }
            return h.col_idx;
        }
    }
    std::size_t idx = builder.add_or_get_column(key_sv, type);
    if (pos >= hints.size()) hints.resize(pos + 1);
    auto& h = hints[pos];
    h.key.assign(key_sv);
    h.type = type;
    h.col_idx = idx;
    h.valid = true;
    return idx;
}

// Append a typed scalar value under `key_sv`. Nested objects/arrays are
// always round-tripped as JSON strings (flattening is one level only).
void append_scalar_or_json(RecordBatchBuilder& builder,
                           std::vector<ArrowKeyHint>& hints, std::size_t& pos,
                           std::string_view key_sv,
                           simdjson::ondemand::value val,
                           simdjson::ondemand::json_type type) {
    switch (type) {
        case simdjson::ondemand::json_type::number: {
            auto num_r = val.get_number();
            if (num_r.error()) break;
            auto num = num_r.value();
            if (num.is_int64()) {
                auto idx = resolve_col_idx(builder, hints, pos++, key_sv,
                                           ColumnType::INT64);
                builder.append_int64(idx, num.get_int64());
            } else if (num.is_uint64()) {
                auto idx = resolve_col_idx(builder, hints, pos++, key_sv,
                                           ColumnType::UINT64);
                builder.append_uint64(idx, num.get_uint64());
            } else {
                auto idx = resolve_col_idx(builder, hints, pos++, key_sv,
                                           ColumnType::DOUBLE);
                builder.append_double(idx, num.get_double());
            }
            break;
        }
        case simdjson::ondemand::json_type::string: {
            auto str_r = val.get_string();
            if (str_r.error()) break;
            auto idx = resolve_col_idx(builder, hints, pos++, key_sv,
                                       ColumnType::STRING);
            builder.append_string(idx, str_r.value());
            break;
        }
        case simdjson::ondemand::json_type::boolean: {
            auto b_r = val.get_bool();
            if (b_r.error()) break;
            auto idx = resolve_col_idx(builder, hints, pos++, key_sv,
                                       ColumnType::BOOL);
            builder.append_bool(idx, b_r.value());
            break;
        }
        case simdjson::ondemand::json_type::null: {
            auto existing = builder.find_column(key_sv);
            if (existing) builder.append_null(*existing);
            ++pos;
            break;
        }
        case simdjson::ondemand::json_type::object:
        case simdjson::ondemand::json_type::array: {
            auto raw_r = val.raw_json();
            auto idx = resolve_col_idx(builder, hints, pos++, key_sv,
                                       ColumnType::STRING);
            if (!raw_r.error()) {
                auto sv = raw_r.value();
                builder.append_string(idx, sv);
            } else {
                builder.append_null(idx);
            }
            break;
        }
        default:
            ++pos;
            break;
    }
}

// Append one Arrow row from an already-parsed simdjson document.
// Dynamic schema: new columns appended as they appear. When flatten_objects
// is true, top-level object values are expanded one level into `parent.child`
// columns; deeper nesting still lands as a JSON string under the flattened
// key. Returns false on error paths so callers can skip the row.
bool arrow_row_from_doc(RecordBatchBuilder& builder,
                        std::vector<ArrowKeyHint>& hints,
                        simdjson::ondemand::document_reference doc,
                        bool flatten_objects = false) {
    auto obj_result = doc.get_object();
    if (obj_result.error()) return false;
    char key_buf[512];
    std::size_t pos = 0;
    for (auto field : obj_result.value()) {
        if (field.error()) continue;
        auto key_r = field.unescaped_key();
        if (key_r.error()) continue;
        auto key_sv = key_r.value();
        auto val_r = field.value();
        if (val_r.error()) continue;
        auto val = val_r.value();
        auto type_r = val.type();
        if (type_r.error()) continue;
        auto type = type_r.value();

        if (flatten_objects && type == simdjson::ondemand::json_type::object) {
            auto nested = val.get_object();
            if (nested.error()) continue;
            for (auto nf : nested.value()) {
                if (nf.error()) continue;
                auto nk_r = nf.unescaped_key();
                if (nk_r.error()) continue;
                auto nk = nk_r.value();
                auto nv_r = nf.value();
                if (nv_r.error()) continue;
                auto nv = nv_r.value();
                auto nt_r = nv.type();
                if (nt_r.error()) continue;
                std::size_t needed = key_sv.size() + 1 + nk.size();
                if (needed >= sizeof(key_buf)) continue;
                std::memcpy(key_buf, key_sv.data(), key_sv.size());
                key_buf[key_sv.size()] = '.';
                std::memcpy(key_buf + key_sv.size() + 1, nk.data(), nk.size());
                append_scalar_or_json(builder, hints, pos,
                                      std::string_view(key_buf, needed), nv,
                                      nt_r.value());
            }
            continue;
        }

        append_scalar_or_json(builder, hints, pos, key_sv, val, type);
    }
    builder.end_row();
    return true;
}

void collect_query_fields(simdjson::ondemand::document_reference doc,
                          const Query& query, common::query::ValueMap& out);

// Run iterate_many over `padded`, build arrow rows, and emit completed
// batches via `yield_one`. Updates `carry` with the truncated tail (if any)
// for the caller to prepend to the next chunk.
template <typename Yield>
void parse_padded_into_arrow(simdjson::ondemand::parser& bulk_parser,
                             simdjson::padded_string& padded,
                             const std::optional<Query>& query, bool flatten,
                             RecordBatchBuilder& builder,
                             ArrowStringArena& arena,
                             std::vector<ArrowKeyHint>& hints,
                             std::size_t batch_size, std::string* carry,
                             Yield&& yield_one) {
    auto docs_r = bulk_parser.iterate_many(padded, 1 << 20, false);
    if (docs_r.error()) {
        if (carry) carry->clear();
        return;
    }
    auto& docs = docs_r.value();
    for (auto it = docs.begin(); it != docs.end(); ++it) {
        auto doc_result = *it;
        if (doc_result.error()) continue;
        auto& doc = doc_result.value();
        if (query) {
            common::query::ValueMap fields;
            collect_query_fields(doc, *query, fields);
            if (!query->evaluate(fields)) continue;
            doc.rewind();
        }
        if (!arrow_row_from_doc(builder, hints, doc, flatten)) continue;
        if (builder.num_rows() >= batch_size) {
            auto result = builder.finish();
            arena.clear();
            if (!builder.is_schema_locked()) builder.lock_schema();
            builder.reset(true);
            builder.reserve(batch_size);
            yield_one(std::move(result));
        }
    }
    if (carry) {
        std::size_t total = padded.size();
        std::size_t truncated = docs.truncated_bytes();
        if (truncated > 0 && truncated <= total) {
            carry->assign(padded.data() + total - truncated,
                          padded.data() + total);
        } else {
            carry->clear();
        }
    }
}

// Build a simdjson-padded buffer containing only the lines in `chunk` that
// pass the line-level prefilter. For queries with no useful prefilter, the
// caller should skip this and feed the raw chunk directly.
std::string collect_matching_lines(std::span<const char> chunk,
                                   const LinePrefilter& prefilter) {
    std::string out;
    out.reserve(chunk.size());
    const char* data = chunk.data();
    std::size_t len = chunk.size();
    std::size_t pos = 0;
    while (pos < len) {
        const void* nl = std::memchr(data + pos, '\n', len - pos);
        std::size_t end_pos = nl ? static_cast<const char*>(nl) - data : len;
        if (end_pos > pos) {
            std::string_view line(data + pos, end_pos - pos);
            if (prefilter.may_match(line)) {
                out.append(line);
                out.push_back('\n');
            }
        }
        pos = end_pos + 1;
    }
    return out;
}

// Extract fields referenced by the query into a ValueMap, walking one level
// of object nesting. Fields not referenced by the query are skipped.
void collect_query_fields(simdjson::ondemand::document_reference doc,
                          const Query& query, common::query::ValueMap& out) {
    auto obj = doc.get_object();
    if (obj.error()) return;
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
                if (!query.references(nk_r.value())) continue;
                out[std::string(nk_r.value())] =
                    ondemand_to_literal(nv_r.value());
            }
        } else if (query.references(key)) {
            out[std::string(key)] = ondemand_to_literal(val);
        }
    }
}

}  // namespace

coro::AsyncGenerator<ArrowExportResult> TraceReader::read_arrow(
    ReadConfig config, std::size_t batch_size) {
    std::optional<Query> query;
    if (!config.query.empty()) {
        auto parsed = Query::from_string(config.query);
        if (!parsed) throw common::query::QueryParseError(parsed.error());
        query = std::move(*parsed);
    }

    // When chunk_prune_only is set, dim_stats already proved every event in
    // the chunk that has the predicate field matches the literal. We still
    // need to skip events that lack the field (e.g., metadata "ph":"M"
    // events lack pid), since the original predicate would reject them.
    std::vector<std::string> presence_check_paths;
    if (query && config.chunk_prune_only) {
        const auto& fset = query->fields();
        presence_check_paths.assign(fset.begin(), fset.end());
    }

    // For AND-of-EQ predicates, evaluate directly against simdjson without
    // ValueMap (avoids wyhash + per-field std::string allocation per row).
    // Falls back to the ValueMap path on unsupported AST shapes.
    std::vector<CompiledEqProbe> compiled_probes;
    bool use_compiled = false;
    if (query && !config.chunk_prune_only) {
        if (auto p = try_compile_eq_probes(query->root())) {
            compiled_probes = std::move(*p);
            use_compiled = !compiled_probes.empty();
        }
    }

    bool flatten = config.flatten_objects;

    if (!has_index_) {
        // Fallback: drive the per-line read_json path and build rows.
        auto json_gen = read_json(config);
        RecordBatchBuilder builder;
        ArrowStringArena arena;
        std::vector<ArrowKeyHint> hints;
        builder.reserve(batch_size);
        while (auto opt = co_await json_gen.next()) {
            if (!arrow_row_from_doc(builder, hints,
                                    simdjson::ondemand::document_reference(
                                        opt->parser->raw_document()),
                                    flatten))
                continue;
            if (builder.num_rows() >= batch_size) {
                co_yield builder.finish();
                arena.clear();
                if (!builder.is_schema_locked()) builder.lock_schema();
                builder.reset(true);
                builder.reserve(batch_size);
            }
        }
        if (builder.num_rows() > 0) {
            co_yield builder.finish();
        }
        co_return;
    }

    // Keep RocksDB alive for the generator's lifetime so per-method opens
    // in GzipIndexer reuse DBManager's cached handle.
    std::optional<indexer::IndexDatabase> db_keep_alive;
    if (has_index_ && !index_path_.empty()) {
        try {
            db_keep_alive.emplace(index_path_,
                                  rocksdb::RocksDatabase::OpenMode::ReadOnly);
        } catch (...) {
        }
    }

    auto reader = create_indexed_reader();
    auto chunk_gen = read_chunks_indexed(
        reader, index_path_, config_.file_path, config, query,
        /*extend_to_line_boundary=*/config.end_at_checkpoint);

    LinePrefilter prefilter = (query && !config.chunk_prune_only)
                                  ? build_prefilter(*query)
                                  : LinePrefilter{};
    bool have_line_prefilter = !prefilter.empty();

    simdjson::ondemand::parser bulk_parser;
    RecordBatchBuilder builder;
    ArrowStringArena arena;
    std::vector<ArrowKeyHint> hints;
    builder.reserve(batch_size);

    auto maybe_flush = [&builder, &arena, batch_size](
                           bool final) -> std::optional<ArrowExportResult> {
        if (builder.num_rows() == 0) return std::nullopt;
        if (!final && builder.num_rows() < batch_size) return std::nullopt;
        auto result = builder.finish();
        arena.clear();
        if (!builder.is_schema_locked()) builder.lock_schema();
        builder.reset(true);
        builder.reserve(batch_size);
        return result;
    };

    bool first_chunk = true;
    while (auto chunk_opt = co_await chunk_gen.next()) {
        auto chunk = *chunk_opt;
        if (chunk.empty()) continue;

        // Work items with start_byte > 0 begin at a deflate-block boundary
        // that is typically mid-line; the previous worker emitted that
        // spanning line via its tail-flush, so drop bytes up to (and
        // including) the first newline in our first chunk.
        if (first_chunk && config.start_byte > 0 &&
            config.start_at_checkpoint) {
            const char* nl = static_cast<const char*>(
                std::memchr(chunk.data(), '\n', chunk.size()));
            if (nl) {
                std::size_t skip =
                    static_cast<std::size_t>(nl - chunk.data()) + 1;
                if (skip < chunk.size()) {
                    chunk = chunk.subspan(skip);
                } else {
                    first_chunk = false;
                    continue;
                }
            }
        }
        first_chunk = false;

        simdjson::padded_string padded;
        if (have_line_prefilter) {
            auto collected = collect_matching_lines(chunk, prefilter);
            if (collected.empty()) continue;
            padded = simdjson::padded_string(std::move(collected));
        } else {
            auto trimmed = strip_ndjson_bookends(
                std::string_view(chunk.data(), chunk.size()));
            if (trimmed.empty()) continue;
            padded = simdjson::padded_string(trimmed);
        }

        auto docs_r = bulk_parser.iterate_many(padded, 1 << 20,
                                               /*allow_comma_separated=*/false);
        if (docs_r.error()) continue;
        auto& docs = docs_r.value();

        for (auto it = docs.begin(); it != docs.end(); ++it) {
            auto doc_result = *it;
            if (doc_result.error()) continue;
            auto& doc = doc_result.value();

            if (query && !config.chunk_prune_only) {
                if (use_compiled) {
                    if (!eval_compiled_eq(compiled_probes, doc)) continue;
                } else {
                    common::query::ValueMap fields;
                    collect_query_fields(doc, *query, fields);
                    if (!query->evaluate(fields)) continue;
                }
                doc.rewind();
            } else if (!presence_check_paths.empty()) {
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
            }

            if (!arrow_row_from_doc(builder, hints, doc, flatten)) continue;

            if (auto flushed = maybe_flush(/*final=*/false)) {
                co_yield std::move(*flushed);
            }
        }
    }

    if (auto flushed = maybe_flush(/*final=*/true)) {
        co_yield std::move(*flushed);
    }
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW

}  // namespace dftracer::utils::utilities::reader
