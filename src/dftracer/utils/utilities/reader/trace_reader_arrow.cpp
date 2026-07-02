#include <dftracer/utils/utilities/reader/trace_reader.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/core/common/string_arena.h>
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/internal/trace_reader_prefilter.h>
#include <dftracer/utils/utilities/reader/internal/trace_reader_shared.h>
#include <simdjson.h>

#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::reader {

using common::query::Query;
using internal::build_prefilter;
using internal::CompiledEqProbe;
using internal::eval_compiled_eq;
using internal::LinePrefilter;
using internal::ondemand_to_literal;
using internal::read_chunks_indexed;
using internal::strip_ndjson_bookends;
using internal::try_compile_eq_probes;

namespace {

using common::arrow::ArrowExportResult;
using common::arrow::ColumnType;
using common::arrow::RecordBatchBuilder;

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
                             RecordBatchBuilder& builder, StringArena& arena,
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
        StringArena arena;
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
    StringArena arena;
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

}  // namespace dftracer::utils::utilities::reader

#endif  // DFTRACER_UTILS_ENABLE_ARROW
