// SPDX-License-Identifier: MIT
#include <dftracer/utils/json/record_parser.h>
#ifndef DFTRACER_UTILS_JSON_JSON_DOC_GUARD_H
#define DFTRACER_UTILS_JSON_JSON_DOC_GUARD_H

#include <dftracer/utils/dstd/hash/value_hash.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <simdjson.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::json {

/// RAII guard that owns a simdjson DOM parser and document.
/// With simdjson, the parser manages document lifetime internally.
struct JsonDocGuard {
    dftracer::utils::json::RecordParser parser;
    bool valid = false;

    JsonDocGuard() = default;

    bool parse(dstd::StringView text) {
        auto result = parser.parse(text.data(), text.size());
        valid = !result.error();
        return valid;
    }

    simdjson::dom::element root() const { return parser.doc.root(); }

    explicit operator bool() const { return valid; }
};

/// The canonical value hash (@ref dstd::hash::value_hash_int64 and its
/// siblings) of an On-Demand string, number or bool for bloom filter
/// insertion; nullopt for an empty string, a null, an array or an object.
inline dstd::Optional<std::uint64_t> ondemand_value_hash(
    simdjson::ondemand::value& val) {
    auto type_result = val.type();
    if (type_result.error()) return dstd::nullopt;

    switch (type_result.value_unsafe()) {
        case simdjson::ondemand::json_type::string: {
            auto s = val.get_string();
            if (s.error() || s.value_unsafe().empty()) return dstd::nullopt;
            return dstd::hash::value_hash_string(s.value_unsafe());
        }
        case simdjson::ondemand::json_type::number: {
            auto u = val.get_uint64();
            if (!u.error())
                return dstd::hash::value_hash_uint64(u.value_unsafe());
            auto i = val.get_int64();
            if (!i.error())
                return dstd::hash::value_hash_int64(i.value_unsafe());
            auto d = val.get_double();
            if (!d.error())
                return dstd::hash::value_hash_double(d.value_unsafe());
            return dstd::nullopt;
        }
        case simdjson::ondemand::json_type::boolean: {
            auto b = val.get_bool();
            if (b.error()) return dstd::nullopt;
            return dstd::hash::value_hash_bool(b.value_unsafe());
        }
        default:
            return dstd::nullopt;
    }
}

}  // namespace dftracer::utils::json

#endif  // DFTRACER_UTILS_JSON_JSON_DOC_GUARD_H
