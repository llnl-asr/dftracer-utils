// SPDX-License-Identifier: MIT
#include <dftracer/utils/json/record_parser.h>
#ifndef DFTRACER_UTILS_JSON_JSON_DOC_GUARD_H
#define DFTRACER_UTILS_JSON_JSON_DOC_GUARD_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <simdjson.h>

#include <cstddef>

namespace dftracer::utils::json {

/// RAII guard that owns a simdjson DOM parser and document.
/// With simdjson, the parser manages document lifetime internally.
struct JsonDocGuard {
    dftracer::utils::json::RecordParser parser;
    bool valid = false;

    JsonDocGuard() = default;

    bool parse(const char* data, std::size_t len) {
        auto result = parser.parse(data, len);
        valid = !result.error();
        return valid;
    }

    simdjson::dom::element root() const { return parser.doc.root(); }

    explicit operator bool() const { return valid; }
};

/// Convert an On-Demand value to string for bloom filter insertion.
/// Handles strings, integers, floats, bools.
inline dstd::String ondemand_value_to_string(simdjson::ondemand::value& val) {
    auto type_result = val.type();
    if (type_result.error()) return {};

    switch (type_result.value_unsafe()) {
        case simdjson::ondemand::json_type::string: {
            auto s = val.get_string();
            return s.error() ? dstd::String{} : dstd::String(s.value_unsafe());
        }
        case simdjson::ondemand::json_type::number: {
            char buf[32];
            auto u = val.get_uint64();
            if (!u.error()) {
                char* end = dstd::to_chars_u64(buf, buf + sizeof(buf),
                                               u.value_unsafe());
                if (end) return dstd::String(buf, end);
            }
            auto i = val.get_int64();
            if (!i.error()) {
                char* end = dstd::to_chars_i64(buf, buf + sizeof(buf),
                                               i.value_unsafe());
                if (end) return dstd::String(buf, end);
            }
            auto d = val.get_double();
            if (!d.error()) {
                return dstd::fixed_text(d.value_unsafe());
            }
            return {};
        }
        case simdjson::ondemand::json_type::boolean: {
            auto b = val.get_bool();
            return b.error()
                       ? dstd::String{}
                       : dstd::String(b.value_unsafe() ? "true" : "false");
        }
        default:
            return {};
    }
}

}  // namespace dftracer::utils::json

#endif  // DFTRACER_UTILS_JSON_JSON_DOC_GUARD_H
