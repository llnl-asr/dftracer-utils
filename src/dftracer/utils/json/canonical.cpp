// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/algorithm.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/json/canonical.h>
#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/json/record_parser.h>

#include <cstdint>
#include <utility>

namespace dftracer::utils::json {

namespace {

void append_int64(dstd::String& out, std::int64_t v) {
    char buf[24];
    if (char* end = dstd::to_chars_i64(buf, buf + sizeof(buf), v))
        out.append(buf, end);
}

void append_uint64(dstd::String& out, std::uint64_t v) {
    char buf[24];
    if (char* end = dstd::to_chars_u64(buf, buf + sizeof(buf), v))
        out.append(buf, end);
}

}  // namespace

void append_canonical_json(dstd::String& out, simdjson::dom::element v) {
    using T = simdjson::dom::element_type;
    switch (v.type()) {
        case T::OBJECT: {
            dstd::Vector<dstd::Pair<dstd::StringView, simdjson::dom::element>>
                members;
            const simdjson::dom::object obj = v.get_object().value_unsafe();
            for (auto kv : obj) members.emplace_back(kv.key, kv.value);
            dstd::stable_sort(
                members.begin(), members.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            out += '{';
            for (std::size_t i = 0; i < members.size(); ++i) {
                if (i) out += ',';
                out += '"';
                append_json_escaped(out, members[i].first);
                out += "\":";
                append_canonical_json(out, members[i].second);
            }
            out += '}';
            return;
        }
        case T::ARRAY: {
            out += '[';
            bool first = true;
            const simdjson::dom::array arr = v.get_array().value_unsafe();
            for (auto el : arr) {
                if (!first) out += ',';
                first = false;
                append_canonical_json(out, el);
            }
            out += ']';
            return;
        }
        case T::STRING:
            out += '"';
            append_json_escaped(out, v.get_string().value_unsafe());
            out += '"';
            return;
        case T::INT64:
            append_int64(out, v.get_int64().value_unsafe());
            return;
        case T::UINT64:
            append_uint64(out, v.get_uint64().value_unsafe());
            return;
        case T::BIGINT:
            out += v.get_bigint().value_unsafe();
            return;
        case T::DOUBLE: {
            char buf[32];
            char* end = dstd::to_chars_double(buf, buf + sizeof(buf),
                                              v.get_double().value_unsafe());
            if (end) out.append(buf, end);
            return;
        }
        case T::BOOL:
            out += v.get_bool().value_unsafe() ? "true" : "false";
            return;
        case T::NULL_VALUE:
            out += "null";
            return;
    }
}

dstd::String canonical_json_text(dstd::StringView text) {
    thread_local dftracer::utils::json::RecordParser parser;
    simdjson::dom::element root;
    if (parser.parse(simdjson::padded_string(text)).get(root) !=
        simdjson::SUCCESS)
        return dstd::String(text);
    dstd::String out;
    append_canonical_json(out, root);
    return out;
}

}  // namespace dftracer::utils::json
