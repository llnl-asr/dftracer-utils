#ifndef DFTRACER_UTILS_UTILITIES_COMMON_JSON_JSON_DOC_GUARD_H
#define DFTRACER_UTILS_UTILITIES_COMMON_JSON_JSON_DOC_GUARD_H

#include <simdjson.h>

#include <string>

namespace dftracer::utils::utilities::common::json {

/// RAII guard that owns a simdjson DOM parser and document.
/// With simdjson, the parser manages document lifetime internally.
struct JsonDocGuard {
    simdjson::dom::parser parser;
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
inline std::string ondemand_value_to_string(simdjson::ondemand::value& val) {
    auto type_result = val.type();
    if (type_result.error()) return {};

    switch (type_result.value()) {
        case simdjson::ondemand::json_type::string: {
            auto s = val.get_string();
            return s.error() ? std::string{} : std::string(s.value());
        }
        case simdjson::ondemand::json_type::number: {
            auto u = val.get_uint64();
            if (!u.error()) return std::to_string(u.value());
            auto i = val.get_int64();
            if (!i.error()) return std::to_string(i.value());
            auto d = val.get_double();
            if (!d.error()) return std::to_string(d.value());
            return {};
        }
        case simdjson::ondemand::json_type::boolean: {
            auto b = val.get_bool();
            return b.error() ? std::string{} : (b.value() ? "true" : "false");
        }
        default:
            return {};
    }
}

}  // namespace dftracer::utils::utilities::common::json

#endif  // DFTRACER_UTILS_UTILITIES_COMMON_JSON_JSON_DOC_GUARD_H
