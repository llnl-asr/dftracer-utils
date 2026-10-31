#include <dftracer/utils/json/record_parser.h>
#ifndef DFTRACER_UTILS_JSON_JSON_DOC_GUARD_H
#define DFTRACER_UTILS_JSON_JSON_DOC_GUARD_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <simdjson.h>

#include <cstddef>
#include <cstdio>

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

    switch (type_result.value()) {
        case simdjson::ondemand::json_type::string: {
            auto s = val.get_string();
            return s.error() ? dstd::String{} : dstd::String(s.value());
        }
        case simdjson::ondemand::json_type::number: {
            char buf[32];
            auto u = val.get_uint64();
            if (!u.error()) {
                char* end =
                    dstd::to_chars_u64(buf, buf + sizeof(buf), u.value());
                if (end) return dstd::String(buf, end);
            }
            auto i = val.get_int64();
            if (!i.error()) {
                char* end =
                    dstd::to_chars_i64(buf, buf + sizeof(buf), i.value());
                if (end) return dstd::String(buf, end);
            }
            auto d = val.get_double();
            if (!d.error()) {
                // %f is what std::to_string(double) prints; 512 bytes holds
                // every magnitude.
                char text[512];
                const int n =
                    std::snprintf(text, sizeof(text), "%f", d.value());
                if (n > 0 && static_cast<std::size_t>(n) < sizeof(text))
                    return dstd::String(text, static_cast<std::size_t>(n));
            }
            return {};
        }
        case simdjson::ondemand::json_type::boolean: {
            auto b = val.get_bool();
            return b.error() ? dstd::String{}
                             : dstd::String(b.value() ? "true" : "false");
        }
        default:
            return {};
    }
}

}  // namespace dftracer::utils::json

#endif  // DFTRACER_UTILS_JSON_JSON_DOC_GUARD_H
