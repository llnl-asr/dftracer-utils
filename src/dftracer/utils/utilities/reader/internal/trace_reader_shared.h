// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_TRACE_READER_SHARED_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_TRACE_READER_SHARED_H

#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/duql/errc.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/json/canonical.h>
#include <dftracer/utils/json/line.h>
#include <dftracer/utils/json/value_access.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <simdjson.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace dftracer::utils::utilities::reader::internal {

// The error a query that does not parse stands for.
inline dstd::Error query_parse_error(const duql::DuqlError& error) {
    return dstd::Error{duql::DuqlErrc::Parse, error.format()};
}

// A generator that yields `error` and ends, for a read that fails before it
// can start.
template <typename T>
coro::AsyncGenerator<dstd::Result<T>> failed_generator(dstd::Error error) {
    co_yield dstd::Result<T>(std::move(error));
}

// Strip a leading `[` and trailing `]` (plus surrounding whitespace) from a
// chunk buffer. These bookends appear in `.pfw.gz` files to keep them
// Perfetto-viewable as JSON arrays, but are not documents of their own. Safe to
// call on any chunk: if the bookends are absent the range is returned
// unchanged.
inline dstd::StringView strip_ndjson_bookends(dstd::StringView bytes) {
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
    return dstd::StringView(s, static_cast<std::size_t>(e - s));
}

// The lines of a padded buffer of newline-delimited JSON, each parsed as its
// own document in place. A line is a document, so a bad line cannot swallow or
// desynchronise the lines around it, and a long line needs no batch capacity.
// The buffer must be a simdjson::padded_string, or otherwise hold
// SIMDJSON_PADDING readable bytes after `len`. Blank lines, a lone bracket
// and a trailing comma are trimmed as on the non-indexed path. A line that
// simdjson refuses for bad UTF-8 or a raw control byte in a string is parsed
// once more from a repaired private copy (@ref json::detail::repair_line).
class PaddedLines {
   public:
    PaddedLines(simdjson::ondemand::parser& parser, const char* base,
                std::size_t len)
        : parser_(parser), base_(base), len_(len) {}

    // The next line that parses as a document, false at the end. `line` is the
    // original bytes of the line; `text` is the bytes that `doc` points into,
    // `line` itself unless the line was repaired. `doc`, `line` and `text` are
    // valid until the next call.
    //
    // Lines that no repair made parsable are counted in @ref failed.
    bool next(simdjson::ondemand::document_reference& doc,
              dstd::StringView& line, dstd::StringView& text) {
        while (pos_ < len_) {
            const char* const from = base_ + pos_;
            const void* const nl = std::memchr(from, '\n', len_ - pos_);
            const std::size_t size =
                nl ? static_cast<std::size_t>(static_cast<const char*>(nl) -
                                              from)
                   : len_ - pos_;
            pos_ += nl ? size + 1 : size;
            const char* start;
            std::size_t trimmed;
            if (!json::trim_and_validate_with_comma(from, size, start, trimmed))
                continue;
            const std::size_t capacity =
                static_cast<std::size_t>(base_ + len_ - start) +
                simdjson::SIMDJSON_PADDING;
            line = dstd::StringView(start, trimmed);
            text = line;
            simdjson::error_code code =
                parser_.iterate(start, trimmed, capacity).get(document_);
            if (json::detail::is_repairable(code)) {
                repaired_.assign(start, trimmed);
                repaired_.resize(trimmed + simdjson::SIMDJSON_PADDING);
                json::detail::repair_line(repaired_.data(), trimmed);
                code =
                    parser_.iterate(repaired_.data(), trimmed, repaired_.size())
                        .get(document_);
                text = dstd::StringView(repaired_.data(), trimmed);
            }
            if (code) {
                ++failed_;
                continue;
            }
            doc = simdjson::ondemand::document_reference(document_);
            return true;
        }
        return false;
    }

    // The lines so far that did not parse as a document.
    std::size_t failed() const noexcept { return failed_; }

   private:
    simdjson::ondemand::parser& parser_;
    const char* base_;
    std::size_t len_;
    std::size_t pos_ = 0;
    std::size_t failed_ = 0;
    simdjson::ondemand::document document_;
    dstd::String repaired_;
};

// Whether `token` is an integer too large for 64 bits. Such a number has no
// cell, as the evaluator reads it as unknown, and is not a syntax error.
// An object line with no closing brace, the check simdjson makes when a read of
// the object starts. Reads only the text, as that read would abandon the
// document.
inline bool is_cropped_object(dstd::StringView line) {
    return !line.empty() && line.front() == '{' && line.back() != '}';
}

inline bool is_bigint_token(dstd::StringView token) {
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t' ||
                              token.back() == '\n' || token.back() == '\r'))
        token.remove_suffix(1);
    if (!token.empty() && token.front() == '-') token.remove_prefix(1);
    if (token.size() < 19 || token.front() == '0') return false;
    return std::all_of(token.begin(), token.end(),
                       [](char c) { return c >= '0' && c <= '9'; });
}

// A field's value for the evaluator: JSON null as a null cell, an object or
// array as its canonical JSON text. False when reading the value hits a
// syntax error; `out` stays empty for a value that has no cell.
inline bool ondemand_to_cell(simdjson::ondemand::value val,
                             dstd::Optional<duql::Cell>& out) {
    const auto type = val.type();
    if (type.error()) return false;
    switch (type.value_unsafe()) {
        case simdjson::ondemand::json_type::string: {
            const auto text = json::detail::get_string_or_raw(val);
            if (!text) return false;
            out = duql::Cell(dstd::String(*text));
            return true;
        }
        case simdjson::ondemand::json_type::number: {
            auto num = val.get_number();
            if (num.error()) {
                if (is_bigint_token(val.raw_json_token())) return true;
                if (num.error() != simdjson::NUMBER_ERROR) return false;
                const auto refused = json::detail::refused_number(
                    json::detail::trim_token(val.raw_json_token()));
                if (!refused) return false;
                out = duql::Cell(*refused);
                return true;
            }
            auto n = num.value_unsafe();
            if (n.is_int64()) {
                out = duql::Cell(n.get_int64());
            } else if (n.is_uint64()) {
                out = duql::Cell(n.get_uint64());
            } else {
                out = duql::Cell(n.get_double());
            }
            return true;
        }
        case simdjson::ondemand::json_type::boolean: {
            auto r = val.get_bool();
            if (r.error()) return false;
            out = duql::Cell(r.value_unsafe());
            return true;
        }
        case simdjson::ondemand::json_type::null:
            out = duql::Cell::null();
            return true;
        case simdjson::ondemand::json_type::object:
        case simdjson::ondemand::json_type::array: {
            const bool array =
                type.value_unsafe() == simdjson::ondemand::json_type::array;
            auto raw = val.raw_json();
            if (raw.error()) return false;
            out = duql::Cell::json(
                json::canonical_json_text(raw.value_unsafe()), array);
            return true;
        }
        default:
            return true;
    }
}

// Whether the query reads `key` as any(key).
inline bool query_reads_any(const duql::Query& query, dstd::StringView key) {
    const auto& paths = query.any_paths();
    return std::binary_search(paths.begin(), paths.end(), key);
}

// Stores the scalar elements of the array `val` under `<key>.<k>`, the keys
// the evaluator reads for any(key). False when the array has a syntax error.
inline bool store_positions(duql::ValueMap& fields, dstd::StringView key,
                            simdjson::ondemand::value val) {
    auto arr = val.get_array();
    if (arr.error()) return false;
    std::size_t k = 0;
    for (auto el : arr.value_unsafe()) {
        const std::size_t pos = k++;
        if (el.error()) return false;
        simdjson::ondemand::value v = el.value_unsafe();
        const auto type = v.type();
        if (type.error()) return false;
        switch (type.value_unsafe()) {
            case simdjson::ondemand::json_type::string:
            case simdjson::ondemand::json_type::number:
            case simdjson::ondemand::json_type::boolean: {
                dstd::Optional<duql::Cell> cell;
                if (!ondemand_to_cell(v, cell)) return false;
                if (cell)
                    fields[dstd::fmt<"{}.{}">(key, pos)] = std::move(*cell);
                break;
            }
            default:
                break;
        }
    }
    return true;
}

// Stores field `key` of a record for the query: an array read as any(key)
// by its positions, any other referenced field as one value (a null as a
// null cell, so an args value of the same name cannot stand in for it).
// False when reading the field hits a syntax error: the caller drops the line.
inline bool store_referenced(duql::ValueMap& fields, const duql::Query& query,
                             dstd::StringView key,
                             simdjson::ondemand::value val,
                             simdjson::ondemand::json_type type) {
    if (type == simdjson::ondemand::json_type::array &&
        query_reads_any(query, key)) {
        return store_positions(fields, key, val);
    }
    if (query.references(key)) {
        dstd::Optional<duql::Cell> cell;
        if (!ondemand_to_cell(val, cell)) return false;
        if (cell) fields[dstd::String(key)] = std::move(*cell);
    }
    return true;
}

// True if the query references any dotted (nested) field, e.g. "args.ret".
// Cheap; compute once per read to gate the dotted-key work below.
inline bool query_references_dotted(const duql::Query& query) {
    for (const auto& f : query.fields()) {
        if (f.find('.') != dstd::StringView::npos) return true;
    }
    return false;
}

// Store a nested field into the ValueMap under whichever form(s) the query
// references: the bare child key (`ret`, the canonical form) and/or the dotted
// path (`args.ret`). `check_dotted` should be query_references_dotted(query),
// hoisted out of the per-event loop. Consumes `val` exactly once. False when
// reading the value hits a syntax error.
inline bool store_referenced_nested(duql::ValueMap& fields,
                                    const duql::Query& query, bool check_dotted,
                                    dstd::StringView parent,
                                    dstd::StringView child,
                                    simdjson::ondemand::value val) {
    const bool want_bare = query.references(child);
    dstd::String dotted;
    bool want_dotted = false;
    if (check_dotted) {
        dotted.reserve(parent.size() + 1 + child.size());
        dotted.append(parent).append(".").append(child);
        want_dotted = query.references(dotted);
    }
    if (!want_bare && !want_dotted) return true;
    const auto type = val.type();
    if (type.error()) return false;
    if (type.value_unsafe() == simdjson::ondemand::json_type::array) {
        // The evaluator also tries the bare key, so the positions of an args
        // array are stored once, under whichever form the query names.
        if (query_reads_any(query, dotted))
            return store_positions(fields, dotted, val);
        if (query_reads_any(query, child))
            return store_positions(fields, child, val);
        return true;
    }
    dstd::Optional<duql::Cell> cell;
    if (!ondemand_to_cell(val, cell)) return false;
    if (!cell) return true;
    // A top-level field of the same name wins, as the evaluator resolves it.
    if (want_bare) fields.try_emplace(dstd::String(child), *cell);
    if (want_dotted) fields[std::move(dotted)] = std::move(*cell);
    return true;
}

// Chunk generator with index-driven pruning. Defined in trace_reader.cpp;
// shared by read_json (core) and read_arrow (Arrow export).
coro::AsyncGenerator<dstd::Result<dstd::Span<const char>>> read_chunks_indexed(
    dstd::SharedPtr<Reader> reader, dstd::String index_path,
    dstd::String file_path, ReadConfig config,
    dstd::Optional<duql::Query> query, bool extend_to_line_boundary = false);

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_TRACE_READER_SHARED_H
