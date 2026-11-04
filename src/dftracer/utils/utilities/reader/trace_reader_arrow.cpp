// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_arena.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/plan/prefilter.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/utilities/arrow/column_builder.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/internal/trace_reader_prefilter.h>
#include <dftracer/utils/utilities/reader/internal/trace_reader_shared.h>
#include <simdjson.h>

#include <cstring>
#include <string>

namespace dftracer::utils::utilities::reader {

using duql::Query;
using internal::CompiledEqProbe;
using internal::eval_compiled_eq;
using internal::read_chunks_indexed;
using internal::strip_ndjson_bookends;
using internal::try_compile_eq_probes;

namespace {

using utilities::arrow::ArrowExportResult;
using utilities::arrow::ColumnType;
using utilities::arrow::RecordBatchBuilder;

struct ArrowKeyHint {
    dstd::String key;
    std::size_t col_idx = 0;
    ColumnType type = ColumnType::Int64;
    bool valid = false;
};

inline std::size_t resolve_col_idx(RecordBatchBuilder& builder,
                                   dstd::Vector<ArrowKeyHint>& hints,
                                   std::size_t pos, dstd::StringView key_sv,
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
// always round-tripped as JSON strings (flattening is one level only). An
// integer that does not fit 64 bits is kept as a string of its exact digits.
// False when the value could not be read and was left out.
bool append_scalar_or_json(RecordBatchBuilder& builder,
                           dstd::Vector<ArrowKeyHint>& hints, std::size_t& pos,
                           dstd::StringView key_sv,
                           simdjson::ondemand::value val,
                           simdjson::ondemand::json_type type) {
    switch (type) {
        case simdjson::ondemand::json_type::number: {
            auto num_r = val.get_number();
            if (num_r.error()) {
                const auto token =
                    json::detail::trim_token(val.raw_json_token());
                if (num_r.error() == simdjson::BIGINT_ERROR ||
                    (num_r.error() == simdjson::NUMBER_ERROR &&
                     json::detail::is_integer_number(token))) {
                    auto idx = resolve_col_idx(builder, hints, pos++, key_sv,
                                               ColumnType::String);
                    builder.append_string(idx, token);
                    break;
                }
                if (num_r.error() != simdjson::NUMBER_ERROR) return false;
                const auto refused = json::detail::refused_number(token);
                if (!refused) return false;
                auto idx = resolve_col_idx(builder, hints, pos++, key_sv,
                                           ColumnType::Double);
                builder.append_double(idx, *refused);
                break;
            }
            auto num = num_r.value();
            if (num.is_int64()) {
                auto idx = resolve_col_idx(builder, hints, pos++, key_sv,
                                           ColumnType::Int64);
                builder.append_int64(idx, num.get_int64());
            } else if (num.is_uint64()) {
                auto idx = resolve_col_idx(builder, hints, pos++, key_sv,
                                           ColumnType::Uint64);
                builder.append_uint64(idx, num.get_uint64());
            } else {
                auto idx = resolve_col_idx(builder, hints, pos++, key_sv,
                                           ColumnType::Double);
                builder.append_double(idx, num.get_double());
            }
            break;
        }
        case simdjson::ondemand::json_type::string: {
            const auto str = json::detail::get_string_or_raw(val);
            if (!str) return false;
            auto idx = resolve_col_idx(builder, hints, pos++, key_sv,
                                       ColumnType::String);
            builder.append_string(idx, *str);
            break;
        }
        case simdjson::ondemand::json_type::boolean: {
            auto b_r = val.get_bool();
            if (b_r.error()) return false;
            auto idx = resolve_col_idx(builder, hints, pos++, key_sv,
                                       ColumnType::Bool);
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
                                       ColumnType::String);
            if (!raw_r.error()) {
                auto sv = raw_r.value();
                builder.append_string(idx, sv);
            } else {
                builder.append_null(idx);
                return false;
            }
            break;
        }
        default:
            ++pos;
            break;
    }
    return true;
}

// Append one Arrow row from an already-parsed simdjson document.
// Dynamic schema: new columns appended as they appear. When flatten_objects
// is true, top-level object values are expanded one level into `parent.child`
// columns; deeper nesting still lands as a JSON string under the flattened
// key. Returns false when the line is no object and has no row. A field that
// cannot be read is left out of its row and sets `damaged`.
bool arrow_row_from_doc(RecordBatchBuilder& builder,
                        dstd::Vector<ArrowKeyHint>& hints,
                        simdjson::ondemand::document_reference doc,
                        bool& damaged, bool flatten_objects = false) {
    auto obj_result = doc.get_object();
    if (obj_result.error()) return false;
    char key_buf[512];
    std::size_t pos = 0;
    for (auto field : obj_result.value()) {
        if (field.error()) {
            damaged = true;
            continue;
        }
        const auto key_r = json::detail::get_key_or_raw(field);
        if (!key_r) {
            damaged = true;
            continue;
        }
        auto key_sv = *key_r;
        auto val_r = field.value();
        if (val_r.error()) {
            damaged = true;
            continue;
        }
        auto val = val_r.value();
        auto type_r = val.type();
        if (type_r.error()) {
            damaged = true;
            continue;
        }
        auto type = type_r.value();

        if (flatten_objects && type == simdjson::ondemand::json_type::object) {
            auto nested = val.get_object();
            if (nested.error()) {
                damaged = true;
                continue;
            }
            for (auto nf : nested.value()) {
                if (nf.error()) {
                    damaged = true;
                    continue;
                }
                const auto nk_r = json::detail::get_key_or_raw(nf);
                if (!nk_r) {
                    damaged = true;
                    continue;
                }
                auto nk = *nk_r;
                auto nv_r = nf.value();
                if (nv_r.error()) {
                    damaged = true;
                    continue;
                }
                auto nv = nv_r.value();
                auto nt_r = nv.type();
                if (nt_r.error()) {
                    damaged = true;
                    continue;
                }
                std::size_t needed = key_sv.size() + 1 + nk.size();
                if (needed >= sizeof(key_buf)) continue;
                std::memcpy(key_buf, key_sv.data(), key_sv.size());
                key_buf[key_sv.size()] = '.';
                std::memcpy(key_buf + key_sv.size() + 1, nk.data(), nk.size());
                if (!append_scalar_or_json(builder, hints, pos,
                                           dstd::StringView(key_buf, needed),
                                           nv, nt_r.value()))
                    damaged = true;
            }
            continue;
        }

        if (!append_scalar_or_json(builder, hints, pos, key_sv, val, type))
            damaged = true;
    }
    builder.end_row();
    return true;
}

bool collect_query_fields(simdjson::ondemand::document_reference doc,
                          const Query& query, bool check_dotted,
                          duql::ValueMap& out);

// Build a simdjson-padded buffer containing only the lines in `chunk` that
// pass the line-level prefilter. For queries with no useful prefilter, the
// caller should skip this and feed the raw chunk directly.
dstd::String collect_matching_lines(dstd::Span<const char> chunk,
                                    const index::plan::Prefilter& prefilter) {
    dstd::String out;
    out.reserve(chunk.size());
    const char* data = chunk.data();
    std::size_t len = chunk.size();
    std::size_t pos = 0;
    while (pos < len) {
        const void* nl = std::memchr(data + pos, '\n', len - pos);
        std::size_t end_pos = nl ? static_cast<const char*>(nl) - data : len;
        if (end_pos > pos) {
            dstd::StringView line(data + pos, end_pos - pos);
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
bool collect_query_fields(simdjson::ondemand::document_reference doc,
                          const Query& query, bool check_dotted,
                          duql::ValueMap& out) {
    auto obj = doc.get_object();
    if (obj.error()) return obj.error() == simdjson::INCORRECT_TYPE;
    for (auto field : obj.value()) {
        if (field.error()) return false;
        const auto key_r = json::detail::get_key_or_raw(field);
        if (!key_r) return false;
        auto val_r = field.value();
        if (val_r.error()) return false;
        auto key = *key_r;
        auto val = val_r.value();
        auto type_r = val.type();
        if (type_r.error()) return false;
        auto type = type_r.value();
        if (type == simdjson::ondemand::json_type::object) {
            auto nested = val.get_object();
            if (nested.error()) return false;
            for (auto nf : nested.value()) {
                if (nf.error()) return false;
                const auto nk_r = json::detail::get_key_or_raw(nf);
                if (!nk_r) return false;
                auto nv_r = nf.value();
                if (nv_r.error()) return false;
                if (!internal::store_referenced_nested(
                        out, query, check_dotted, key, *nk_r, nv_r.value()))
                    return false;
            }
        } else {
            if (!internal::store_referenced(out, query, key, val, type))
                return false;
        }
    }
    return true;
}

}  // namespace

coro::AsyncGenerator<ArrowExportResult> TraceReader::read_arrow(
    ReadConfig config, std::size_t batch_size) {
    dstd::Optional<Query> query;
    if (!config.query.empty()) {
        auto parsed = Query::from_string(config.query);
        if (!parsed) throw duql::DuqlParseError(parsed.error());
        query = std::move(*parsed);
    }

    // When chunk_prune_only is set, dim_stats already proved every event in
    // the chunk that has the predicate field matches the literal. We still
    // need to skip events that lack the field (e.g., metadata "ph":"M"
    // events lack pid), since the original predicate would reject them.
    dstd::Vector<dstd::String> presence_check_paths;
    if (query && config.chunk_prune_only) {
        const auto& fset = query->fields();
        presence_check_paths.assign(fset.begin(), fset.end());
    }

    const bool query_has_dotted =
        query && internal::query_references_dotted(*query);

    // For AND-of-EQ predicates, evaluate directly against simdjson without
    // ValueMap (avoids wyhash + per-field std::string allocation per row).
    // Falls back to the ValueMap path on unsupported AST shapes.
    dstd::Vector<CompiledEqProbe> compiled_probes;
    bool use_compiled = false;
    if (query && !config.chunk_prune_only) {
        if (auto p = try_compile_eq_probes(
                duql::detail::QueryAccess::root(*query))) {
            compiled_probes = std::move(*p);
            use_compiled = !compiled_probes.empty();
        }
    }

    bool flatten = config.flatten_objects;

    if (!has_index_) {
        // Fallback: drive the per-line read_json path and build rows.
        auto json_gen = read_json(config);
        RecordBatchBuilder builder;
        dstd::StringArena arena;
        dstd::Vector<ArrowKeyHint> hints;
        std::size_t dropped = 0;
        builder.reserve(batch_size);
        for (;;) {
            auto opt = co_await json_gen.next();
            if (!opt) break;
            bool damaged = false;
            if (!arrow_row_from_doc(builder, hints,
                                    simdjson::ondemand::document_reference(
                                        opt->parser->raw_document()),
                                    damaged, flatten)) {
                // read_json has counted a cropped object.
                if (!internal::is_cropped_object(opt->parser->text()))
                    count_dropped(dropped);
                continue;
            }
            if (damaged) count_dropped(dropped);
            if (builder.num_rows() >= batch_size) {
                co_yield builder.finish();
                arena.clear();
                if (!builder.is_schema_locked()) builder.lock_schema();
                builder.reset(true);
                builder.reserve(batch_size);
            }
        }
        warn_dropped(dropped);
        if (builder.num_rows() > 0) {
            co_yield builder.finish();
        }
        co_return;
    }

    auto reader = create_indexed_reader();
    auto chunk_gen = read_chunks_indexed(
        reader, index_path_, config_.file_path, config, query,
        /*extend_to_line_boundary=*/config.end_at_checkpoint);

    index::plan::Prefilter prefilter = (query && !config.chunk_prune_only)
                                           ? index::plan::Prefilter(*query)
                                           : index::plan::Prefilter{};
    bool have_line_prefilter = !prefilter.empty();

    // An expression may read a whole object or array, which the field map
    // cannot hold, so it runs on the record's DOM.
    dstd::StringArena event_strings;
    simdjson::ondemand::parser bulk_parser;
    RecordBatchBuilder builder;
    dstd::StringArena arena;
    dstd::Vector<ArrowKeyHint> hints;
    builder.reserve(batch_size);
    std::size_t dropped = 0;

    auto maybe_flush = [&builder, &arena, batch_size](
                           bool final) -> dstd::Optional<ArrowExportResult> {
        if (builder.num_rows() == 0) return dstd::nullopt;
        if (!final && builder.num_rows() < batch_size) return dstd::nullopt;
        auto result = builder.finish();
        arena.clear();
        if (!builder.is_schema_locked()) builder.lock_schema();
        builder.reset(true);
        builder.reserve(batch_size);
        return result;
    };

    bool first_chunk = true;
    for (;;) {
        auto chunk_opt = co_await chunk_gen.next();
        if (!chunk_opt) break;
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
                dstd::StringView(chunk.data(), chunk.size()));
            if (trimmed.empty()) continue;
            padded = simdjson::padded_string(trimmed);
        }

        internal::PaddedLines docs(bulk_parser, padded.data(), padded.size());
        simdjson::ondemand::document_reference doc;
        dstd::StringView src;
        dstd::StringView text;
        while (docs.next(doc, src, text)) {
            if (query && !config.chunk_prune_only) {
                if (use_compiled) {
                    bool broken = false;
                    if (!eval_compiled_eq(compiled_probes, doc, broken)) {
                        if (broken) count_dropped(dropped);
                        continue;
                    }
                } else if (query->has_expressions()) {
                    event_strings.clear();
                    const duql::OnDemandEvent event(doc, text, event_strings);
                    if (!duql::evaluate(duql::detail::QueryAccess::root(*query),
                                        event)) {
                        if (event.broken) count_dropped(dropped);
                        continue;
                    }
                } else {
                    duql::ValueMap fields;
                    // A syntax error stops the read and drops the line.
                    if (!collect_query_fields(doc, *query, query_has_dotted,
                                              fields)) {
                        count_dropped(dropped);
                        continue;
                    }
                    if (!query->evaluate(fields)) continue;
                }
                doc.rewind();
            } else if (!presence_check_paths.empty()) {
                bool all_present = true;
                for (const auto& path : presence_check_paths) {
                    auto fld = doc.find_field_unordered(path);
                    if (fld.error()) {
                        if (fld.error() != simdjson::NO_SUCH_FIELD)
                            count_dropped(dropped);
                        all_present = false;
                        break;
                    }
                }
                if (!all_present) continue;
                doc.rewind();
            }

            bool damaged = false;
            if (!arrow_row_from_doc(builder, hints, doc, damaged, flatten)) {
                count_dropped(dropped);
                continue;
            }
            if (damaged) count_dropped(dropped);

            if (auto flushed = maybe_flush(/*final=*/false)) {
                co_yield std::move(*flushed);
            }
        }
        count_dropped(dropped, docs.failed());
    }
    warn_dropped(dropped);

    if (auto flushed = maybe_flush(/*final=*/true)) {
        co_yield std::move(*flushed);
    }
}

}  // namespace dftracer::utils::utilities::reader

#endif  // DFTRACER_UTILS_ENABLE_ARROW
