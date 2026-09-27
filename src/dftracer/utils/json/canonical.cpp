#include <dftracer/utils/core/common/to_chars.h>
#include <dftracer/utils/json/canonical.h>
#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/json/record_parser.h>

#include <algorithm>
#include <vector>

namespace dftracer::utils::json {

void append_canonical_json(std::string& out, simdjson::dom::element v) {
    using T = simdjson::dom::element_type;
    switch (v.type()) {
        case T::OBJECT: {
            std::vector<std::pair<std::string_view, simdjson::dom::element>>
                members;
            const simdjson::dom::object obj = v.get_object().value_unsafe();
            for (auto kv : obj) members.emplace_back(kv.key, kv.value);
            std::stable_sort(
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
            out += std::to_string(v.get_int64().value_unsafe());
            return;
        case T::UINT64:
            out += std::to_string(v.get_uint64().value_unsafe());
            return;
        case T::BIGINT:
            out += v.get_bigint().value_unsafe();
            return;
        case T::DOUBLE: {
            char buf[32];
            char* end = to_chars_double(buf, buf + sizeof(buf),
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

std::string canonical_json_text(std::string_view text) {
    thread_local dftracer::utils::json::RecordParser parser;
    simdjson::dom::element root;
    if (parser.parse(simdjson::padded_string(text)).get(root) !=
        simdjson::SUCCESS)
        return std::string(text);
    std::string out;
    append_canonical_json(out, root);
    return out;
}

}  // namespace dftracer::utils::json
