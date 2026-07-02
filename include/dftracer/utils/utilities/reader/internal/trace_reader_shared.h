#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_TRACE_READER_SHARED_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_TRACE_READER_SHARED_H

#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <simdjson.h>

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

inline common::query::LiteralValue ondemand_to_literal(
    simdjson::ondemand::value val) {
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

// Chunk generator with index-driven pruning. Defined in trace_reader.cpp;
// shared by read_json (core) and read_arrow (Arrow export).
coro::AsyncGenerator<std::span<const char>> read_chunks_indexed(
    std::shared_ptr<Reader> reader, std::string index_path,
    std::string file_path, ReadConfig config,
    std::optional<common::query::Query> query,
    bool extend_to_line_boundary = false);

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_TRACE_READER_SHARED_H
