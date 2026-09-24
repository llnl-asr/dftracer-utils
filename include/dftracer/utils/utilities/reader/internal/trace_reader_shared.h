#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_TRACE_READER_SHARED_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_TRACE_READER_SHARED_H

#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/query/query.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <simdjson.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace dftracer::utils::utilities::reader::internal {

// Strip a leading `[` and trailing `]` (plus surrounding whitespace) from a
// chunk buffer. These bookends appear in `.pfw.gz` files to keep them
// Perfetto-viewable as JSON arrays, but break simdjson iterate_many which
// expects whitespace-separated NDJSON. Safe to call on any chunk: if the
// bookends are absent the range is returned unchanged.
inline std::string_view strip_ndjson_bookends(std::string_view bytes) {
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

inline query::LiteralValue ondemand_to_literal(simdjson::ondemand::value val) {
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

// Whether the query reads `key` as any(key).
inline bool query_reads_any(const query::Query& query, std::string_view key) {
    const auto& paths = query.any_paths();
    return std::binary_search(paths.begin(), paths.end(), key);
}

// Stores the scalar elements of the array `val` under `<key>.<k>`, the keys
// the evaluator reads for any(key).
inline void store_positions(query::ValueMap& fields, std::string_view key,
                            simdjson::ondemand::value val) {
    auto arr = val.get_array();
    if (arr.error()) return;
    std::size_t k = 0;
    for (auto el : arr.value_unsafe()) {
        const std::size_t pos = k++;
        if (el.error()) continue;
        simdjson::ondemand::value v = el.value_unsafe();
        const auto type = v.type();
        if (type.error()) continue;
        switch (type.value_unsafe()) {
            case simdjson::ondemand::json_type::string:
            case simdjson::ondemand::json_type::number:
            case simdjson::ondemand::json_type::boolean:
                fields[std::string(key) + "." + std::to_string(pos)] =
                    ondemand_to_literal(v);
                break;
            default:
                break;
        }
    }
}

// Stores field `key` of a record for the query: an array read as any(key)
// by its positions, any other referenced field as one value.
inline void store_referenced(query::ValueMap& fields, const query::Query& query,
                             std::string_view key,
                             simdjson::ondemand::value val,
                             simdjson::ondemand::json_type type) {
    if (type == simdjson::ondemand::json_type::array &&
        query_reads_any(query, key)) {
        store_positions(fields, key, val);
    } else if (query.references(key)) {
        fields[std::string(key)] = ondemand_to_literal(val);
    }
}

// True if the query references any dotted (nested) field, e.g. "args.ret".
// Cheap; compute once per read to gate the dotted-key work below.
inline bool query_references_dotted(const query::Query& query) {
    for (const auto& f : query.fields()) {
        if (f.find('.') != std::string_view::npos) return true;
    }
    return false;
}

// Store a nested field into the ValueMap under whichever form(s) the query
// references: the bare child key (`ret`, the canonical form) and/or the dotted
// path (`args.ret`). `check_dotted` should be query_references_dotted(query),
// hoisted out of the per-event loop. Consumes `val` exactly once.
inline void store_referenced_nested(query::ValueMap& fields,
                                    const query::Query& query,
                                    bool check_dotted, std::string_view parent,
                                    std::string_view child,
                                    simdjson::ondemand::value val) {
    bool want_bare = query.references(child);
    std::string dotted;
    bool want_dotted = false;
    if (check_dotted) {
        dotted.reserve(parent.size() + 1 + child.size());
        dotted.append(parent).append(".").append(child);
        want_dotted = query.references(dotted);
    }
    if (!want_bare && !want_dotted) return;
    if (val.type().value_unsafe() == simdjson::ondemand::json_type::array) {
        // The evaluator also tries the bare key, so the positions of an args
        // array are stored once, under whichever form the query names.
        if (query_reads_any(query, dotted))
            store_positions(fields, dotted, val);
        else if (query_reads_any(query, child))
            store_positions(fields, child, val);
        return;
    }
    auto lit = ondemand_to_literal(val);
    // A top-level field of the same name wins, as the evaluator resolves it.
    if (want_bare) fields.try_emplace(std::string(child), lit);
    if (want_dotted) fields[std::move(dotted)] = std::move(lit);
}

// Chunk generator with index-driven pruning. Defined in trace_reader.cpp;
// shared by read_json (core) and read_arrow (Arrow export).
coro::AsyncGenerator<std::span<const char>> read_chunks_indexed(
    std::shared_ptr<Reader> reader, std::string index_path,
    std::string file_path, ReadConfig config, std::optional<query::Query> query,
    bool extend_to_line_boundary = false);

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_TRACE_READER_SHARED_H
