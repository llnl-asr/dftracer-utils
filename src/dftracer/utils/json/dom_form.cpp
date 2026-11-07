// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/json/dom_form.h>
#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/json/value_access.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace dftracer::utils::json {

namespace {

constexpr std::size_t MAX_DEPTH = common::constants::reader::MAX_READ_DEPTH;
constexpr dstd::StringView LARGEST_DOUBLE = "1.7976931348623157e308";

void write_string(dstd::String& out, dstd::StringView text) {
    out.push_back('"');
    append_json_escaped(out, text);
    out.push_back('"');
}

void write_double(dstd::String& out, Value& v) {
    const dstd::StringView token = v.raw();
    if (!token.empty() && token.front() != '-' && token.size() <= 20 &&
        detail::is_integer_number(token))
        if (dstd::from_chars<std::uint64_t>(token)) {
            out.append(token);
            return;
        }
    const auto d = v.as_double();
    if (!d) {
        out += "null";
        return;
    }
    if (std::isinf(*d)) {
        if (*d < 0) out.push_back('-');
        out.append(LARGEST_DOUBLE);
        return;
    }
    char buf[32];
    const auto r = dstd::to_chars(buf, buf + sizeof(buf), *d);
    dstd::StringView text(buf, static_cast<std::size_t>(r.ptr - buf));
    if (detail::is_integer_number(text) && text.size() >= 20) {
        if (text.front() == '-') {
            out.push_back('-');
            text.remove_prefix(1);
        }
        const std::size_t exponent = text.size() - 1;
        std::size_t last = text.size();
        while (last > 1 && text[last - 1] == '0') --last;
        out.push_back(text[0]);
        out.push_back('.');
        if (last > 1)
            out.append(text.substr(1, last - 1));
        else
            out.push_back('0');
        out.push_back('e');
        char digits[8];
        const auto e =
            dstd::to_chars(digits, digits + sizeof(digits), exponent);
        out.append(digits, static_cast<std::size_t>(e.ptr - digits));
        return;
    }
    out.append(text);
}

void write_value(dstd::String& out, Value& v, std::size_t depth) {
    switch (v.type()) {
        case Value::Type::Object: {
            if (depth >= MAX_DEPTH) {
                write_string(out, v.raw());
                return;
            }
            out.push_back('{');
            bool first = true;
            v.for_each_member([&](dstd::StringView key, Value child) {
                if (!first) out.push_back(',');
                first = false;
                write_string(out, key);
                out.push_back(':');
                write_value(out, child, depth + 1);
            });
            out.push_back('}');
            return;
        }
        case Value::Type::Array: {
            if (depth >= MAX_DEPTH) {
                write_string(out, v.raw());
                return;
            }
            out.push_back('[');
            bool first = true;
            v.for_each_element([&](Value child) {
                if (!first) out.push_back(',');
                first = false;
                write_value(out, child, depth + 1);
            });
            out.push_back(']');
            return;
        }
        case Value::Type::String: {
            const auto s = v.as_string();
            if (s)
                write_string(out, *s);
            else
                out += "null";
            return;
        }
        case Value::Type::Int: {
            const auto i = v.as_int();
            if (i)
                out += dstd::int_text(*i);
            else
                out += "null";
            return;
        }
        case Value::Type::Double:
            write_double(out, v);
            return;
        case Value::Type::Bool: {
            const auto b = v.as_bool();
            out += b && *b ? "true" : "false";
            return;
        }
        case Value::Type::Null:
            out += "null";
            return;
    }
}

}  // namespace

bool dom_form(dstd::StringView line, dstd::String& out) {
    thread_local Document document;
    out.clear();
    auto parsed = document.parse(line);
    if (!parsed) return false;
    const std::size_t lead = line.find_first_not_of(" \t\r\n");
    const std::size_t used = lead + parsed->raw().size();
    if (used < line.size() &&
        line.find_first_not_of(" \t\r\n", used) != dstd::StringView::npos)
        return false;
    write_value(out, *parsed, 0);
    if (detail::ValueAccess::broken(*parsed)) {
        out.clear();
        return false;
    }
    return true;
}

simdjson::simdjson_result<simdjson::dom::element> parse_refused(
    simdjson::dom::parser& parser, dstd::StringView line) {
    thread_local dstd::String form;
    if (!dom_form(line, form))
        return simdjson::simdjson_result<simdjson::dom::element>(
            simdjson::TAPE_ERROR);
    return parser.parse(form.data(), form.size());
}

}  // namespace dftracer::utils::json
