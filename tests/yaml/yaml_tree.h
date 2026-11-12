// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_YAML_TREE_H
#define DFTRACER_UTILS_TESTS_YAML_TREE_H

#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/yaml/document.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace dftu_yaml_test {

// A tree of values, the reference that a reader is compared with: the tree of
// a JSON text of the YAML test suite, or the tree that a fuzz case is written
// from.
struct Tree {
    enum class Kind : std::uint8_t { Null, Bool, Number, String, Seq, Map };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    // A string, or the spelling of a number.
    dftracer::utils::dstd::String text;
    // The elements of a sequence, or the values of a map.
    dftracer::utils::dstd::Vector<Tree> items;
    // The keys of a map, one for each item.
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> keys;
};

inline Tree make_string(dftracer::utils::dstd::StringView text) {
    Tree t;
    t.kind = Tree::Kind::String;
    t.text = dftracer::utils::dstd::String(text);
    return t;
}

// A tree from a JSON value, which the walk reads once.
inline Tree tree_of_json(dftracer::utils::json::Value value) {
    using dftracer::utils::dstd::String;
    using dftracer::utils::dstd::StringView;
    using dftracer::utils::json::Value;
    Tree t;
    switch (value.type()) {
        case Value::Type::Object:
            t.kind = Tree::Kind::Map;
            value.for_each_member([&](StringView key, Value member) {
                t.keys.push_back(String(key));
                t.items.push_back(tree_of_json(std::move(member)));
            });
            break;
        case Value::Type::Array:
            t.kind = Tree::Kind::Seq;
            value.for_each_element([&](Value element) {
                t.items.push_back(tree_of_json(std::move(element)));
            });
            break;
        case Value::Type::String:
            t.kind = Tree::Kind::String;
            t.text = String(*value.as_string());
            break;
        case Value::Type::Int:
        case Value::Type::Double:
            t.kind = Tree::Kind::Number;
            t.text = String(value.raw());
            t.number = value.as_double().value_or(0);
            break;
        case Value::Type::Bool:
            t.kind = Tree::Kind::Bool;
            t.boolean = value.as_bool().value_or(false);
            break;
        default:
            t.kind = Tree::Kind::Null;
            break;
    }
    return t;
}

// The texts of the JSON values that a stream holds, in order: each is an
// object, an array, a string or a bare word.
inline bool split_json_stream(
    dftracer::utils::dstd::StringView text,
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>& out) {
    using dftracer::utils::dstd::String;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++i;
            continue;
        }
        const std::size_t start = i;
        if (c == '{' || c == '[') {
            int depth = 0;
            bool in_string = false;
            for (; i < text.size(); ++i) {
                const char d = text[i];
                if (in_string) {
                    if (d == '\\')
                        ++i;
                    else if (d == '"')
                        in_string = false;
                } else if (d == '"') {
                    in_string = true;
                } else if (d == '{' || d == '[') {
                    ++depth;
                } else if (d == '}' || d == ']') {
                    if (--depth == 0) {
                        ++i;
                        break;
                    }
                }
            }
        } else if (c == '"') {
            for (++i; i < text.size() && text[i] != '"'; ++i)
                if (text[i] == '\\') ++i;
            if (i >= text.size()) return false;
            ++i;
        } else {
            while (i < text.size() && text[i] != ' ' && text[i] != '\n' &&
                   text[i] != '\t' && text[i] != '\r')
                ++i;
        }
        out.push_back(String(text.substr(start, i - start)));
    }
    return true;
}

// The trees of the JSON values of a stream. False when the text is not JSON.
inline bool trees_of_json(dftracer::utils::dstd::StringView text,
                          dftracer::utils::dstd::Vector<Tree>& out) {
    using dftracer::utils::dstd::String;
    dftracer::utils::dstd::Vector<String> values;
    if (!split_json_stream(text, values)) return false;
    dftracer::utils::json::Document doc;
    for (const String& value : values) {
        const String wrapped = "[" + value + "]";
        auto root = doc.parse(wrapped);
        if (!root) return false;
        Tree wrapper = tree_of_json(std::move(*root));
        if (wrapper.items.size() != 1) return false;
        out.push_back(std::move(wrapper.items[0]));
    }
    return true;
}

// Whether `text` is a number of the YAML 1.2 core schema, and its value.
inline bool core_number(dftracer::utils::dstd::StringView text, double& value) {
    using dftracer::utils::dstd::StringView;
    if (text == ".inf" || text == ".Inf" || text == ".INF" || text == "+.inf" ||
        text == "+.Inf" || text == "+.INF") {
        value = std::numeric_limits<double>::infinity();
        return true;
    }
    if (text == "-.inf" || text == "-.Inf" || text == "-.INF") {
        value = -std::numeric_limits<double>::infinity();
        return true;
    }
    if (text == ".nan" || text == ".NaN" || text == ".NAN") {
        value = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    if (text.size() > 2 && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'o')) {
        const int base = text[1] == 'x' ? 16 : 8;
        double v = 0;
        for (std::size_t i = 2; i < text.size(); ++i) {
            const char c = text[i];
            int digit = -1;
            if (c >= '0' && c <= '9')
                digit = c - '0';
            else if (base == 16 && c >= 'a' && c <= 'f')
                digit = c - 'a' + 10;
            else if (base == 16 && c >= 'A' && c <= 'F')
                digit = c - 'A' + 10;
            if (digit < 0 || digit >= base) return false;
            v = v * base + digit;
        }
        value = v;
        return true;
    }
    StringView body = text;
    if (!body.empty() && (body[0] == '+' || body[0] == '-'))
        body.remove_prefix(1);
    if (body.empty()) return false;
    std::size_t i = 0;
    std::size_t digits = 0;
    while (i < body.size() && body[i] >= '0' && body[i] <= '9') ++i, ++digits;
    if (i < body.size() && body[i] == '.') {
        ++i;
        while (i < body.size() && body[i] >= '0' && body[i] <= '9')
            ++i, ++digits;
    }
    if (digits == 0) return false;
    if (i < body.size() && (body[i] == 'e' || body[i] == 'E')) {
        ++i;
        if (i < body.size() && (body[i] == '+' || body[i] == '-')) ++i;
        std::size_t exp_digits = 0;
        while (i < body.size() && body[i] >= '0' && body[i] <= '9')
            ++i, ++exp_digits;
        if (exp_digits == 0) return false;
    }
    if (i != body.size()) return false;
    StringView number = text;
    if (!number.empty() && number[0] == '+') number.remove_prefix(1);
    auto parsed = dftracer::utils::dstd::from_chars<double>(number);
    if (!parsed) return false;
    value = *parsed;
    return true;
}

// Whether the value `v` is the tree `t`: a map by its keys and their values,
// a sequence in order, and a scalar by its text, where a YAML scalar of the
// core schema stands for a JSON number, a bool or a null. On a difference,
// `why` says where.
inline bool matches(const Tree& t, dftracer::utils::yaml::Value v,
                    dftracer::utils::dstd::String& why, int depth = 0) {
    using dftracer::utils::dstd::String;
    using dftracer::utils::dstd::StringView;
    using dftracer::utils::yaml::Value;
    if (depth > 400) {
        why = "nested deeper than 400 levels";
        return false;
    }
    switch (t.kind) {
        case Tree::Kind::Null:
            if (v.is_null()) return true;
            why = "expected null";
            return false;
        case Tree::Kind::Bool: {
            const StringView raw = v.raw();
            const bool is_true =
                raw == "true" || raw == "True" || raw == "TRUE";
            const bool is_false =
                raw == "false" || raw == "False" || raw == "FALSE";
            if (v.is_string() &&
                ((t.boolean && is_true) || (!t.boolean && is_false)))
                return true;
            why = dftracer::utils::dstd::fmt<"expected bool {}, got '{}'">(
                t.boolean, raw);
            return false;
        }
        case Tree::Kind::Number: {
            double got = 0;
            if (v.is_string() && core_number(v.raw(), got) &&
                (got == t.number || (std::isnan(got) && std::isnan(t.number))))
                return true;
            why = dftracer::utils::dstd::fmt<"expected number {}, got '{}'">(
                t.text, v.raw());
            return false;
        }
        case Tree::Kind::String:
            if (v.is_string() && v.raw() == StringView(t.text)) return true;
            why = dftracer::utils::dstd::fmt<"expected string '{}', got '{}'">(
                t.text, v.is_string() ? v.raw() : StringView("<not a string>"));
            return false;
        case Tree::Kind::Seq: {
            if (!v.is_array()) {
                why = "expected a sequence";
                return false;
            }
            std::size_t i = 0;
            bool ok = true;
            v.for_each_element([&](Value element) {
                if (!ok) return;
                if (i >= t.items.size()) {
                    ++i;
                    ok = false;
                    why = "more elements than expected";
                    return;
                }
                ok = matches(t.items[i], element, why, depth + 1);
                ++i;
            });
            if (ok && i != t.items.size()) {
                why =
                    dftracer::utils::dstd::fmt<"expected {} elements, got {}">(
                        t.items.size(), i);
                return false;
            }
            return ok;
        }
        case Tree::Kind::Map: {
            if (!v.is_object()) {
                why = "expected a map";
                return false;
            }
            std::size_t count = 0;
            bool ok = true;
            v.for_each_member([&](StringView key, Value member) {
                if (!ok) return;
                ++count;
                for (std::size_t i = 0; i < t.keys.size(); ++i) {
                    if (StringView(t.keys[i]) != key) continue;
                    ok = matches(t.items[i], member, why, depth + 1);
                    if (!ok) why = "key '" + String(key) + "': " + why;
                    return;
                }
                ok = false;
                why = "unexpected key '" + String(key) + "'";
            });
            if (ok && count != t.keys.size()) {
                why = dftracer::utils::dstd::fmt<"expected {} members, got {}">(
                    t.keys.size(), count);
                return false;
            }
            if (ok) {
                for (std::size_t i = 0; i < t.keys.size() && ok; ++i) {
                    auto found = v.find(t.keys[i]);
                    if (!found) {
                        ok = false;
                        why = "find misses key '" + t.keys[i] + "'";
                    } else {
                        ok = matches(t.items[i], *found, why, depth + 1);
                    }
                }
            }
            return ok;
        }
    }
    return true;
}

}  // namespace dftu_yaml_test

#endif  // DFTRACER_UTILS_TESTS_YAML_TREE_H
