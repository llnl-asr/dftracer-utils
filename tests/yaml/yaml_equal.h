// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_YAML_EQUAL_H
#define DFTRACER_UTILS_TESTS_YAML_EQUAL_H

#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/yaml/document.h>
#include <dftracer/utils/yaml/document_test.h>
#include <dftracer/utils/yaml/line_index.h>

#include <cstdint>
#include <cstring>

namespace dftu_yaml_test {

// A text of a value tree with everything a reader can say about it: the kind,
// the text of a string, each typed reading, and the lookup of each key.
inline void full_dump(dftracer::utils::yaml::Value value,
                      dftracer::utils::dstd::String& out, int depth = 0) {
    using dftracer::utils::dstd::StringView;
    using dftracer::utils::yaml::Value;
    if (depth > 200) {
        out += "<deep>";
        return;
    }
    if (value.is_object()) {
        out += '{';
        value.for_each_member([&](StringView key, Value member) {
            out += key;
            out += ':';
            full_dump(member, out, depth + 1);
            auto found = value.find(key);
            out += found ? '=' : '!';
            if (found) full_dump(*found, out, depth + 1);
            out += ',';
        });
        out += '}';
    } else if (value.is_array()) {
        out += '[';
        value.for_each_element([&](Value element) {
            full_dump(element, out, depth + 1);
            out += ',';
        });
        out += ']';
    } else if (value.is_string()) {
        out += '\'';
        out += value.raw();
        out += '\'';
        if (auto v = value.as_int())
            out += dftracer::utils::dstd::fmt<"i{}">(*v);
        if (auto v = value.as_uint())
            out += dftracer::utils::dstd::fmt<"u{}">(*v);
        if (auto v = value.as_double()) {
            std::uint64_t bits = 0;
            const double d = *v;
            std::memcpy(&bits, &d, sizeof(bits));
            out += dftracer::utils::dstd::fmt<"d{}">(bits);
        }
        if (auto v = value.as_bool()) out += *v ? "T" : "F";
        if (auto s = value.as_string()) out += (*s == value.raw()) ? "s" : "S";
    } else {
        out += value.is_null() ? "~" : "?";
    }
}

inline dftracer::utils::dstd::String full_dump(
    dftracer::utils::yaml::Value value) {
    dftracer::utils::dstd::String out;
    full_dump(value, out);
    return out;
}

struct Both {
    bool line_accepted = false;
    /// The line index found the text invalid and the parse failed with it.
    bool line_invalid = false;
    bool tape_ok = false;
    dftracer::utils::dstd::String line_dump;
    dftracer::utils::dstd::String tape_dump;
    dftracer::utils::dstd::String line_error;
    dftracer::utils::dstd::String tape_error;
};

// The line of an error message `name:line:col: reason`, 0 when it has none.
inline std::uint64_t error_line(dftracer::utils::dstd::StringView message) {
    const std::size_t first = message.find(':');
    if (first == dftracer::utils::dstd::StringView::npos) return 0;
    std::uint64_t line = 0;
    for (std::size_t i = first + 1; i < message.size(); ++i) {
        if (message[i] < '0' || message[i] > '9') break;
        line = line * 10 + static_cast<std::uint64_t>(message[i] - '0');
    }
    return line;
}

// The reason of an error message `name:line:col: reason`.
inline dftracer::utils::dstd::StringView error_reason(
    dftracer::utils::dstd::StringView message) {
    const std::size_t at = message.find(": ");
    if (at == dftracer::utils::dstd::StringView::npos) return message;
    return message.substr(at + 2);
}

// Parses `text` as it is and with the tape reader forced. `line_invalid` says
// that the error of the first parse came from the line index.
inline Both parse_both(dftracer::utils::dstd::StringView text) {
    using dftracer::utils::dstd::String;
    Both both;
    dftracer::utils::yaml::LineIndex probe;
    const std::size_t bom =
        dftracer::utils::yaml::bom_size(text.data(), text.size());
    const bool line_spoke = probe.build(text.data() + bom, text.size() - bom) ==
                            dftracer::utils::yaml::BuildResult::Invalid;
    auto lines = dftracer::utils::yaml::Document::parse(String(text), "t");
    dftracer::utils::yaml::detail::set_force_tape_for_test(true);
    auto tape = dftracer::utils::yaml::Document::parse(String(text), "t");
    dftracer::utils::yaml::detail::set_force_tape_for_test(false);
    both.tape_ok = tape.has_value();
    if (tape) {
        both.tape_dump = full_dump(tape->root());
    } else {
        both.tape_error = String(tape.error().message());
    }
    if (lines) {
        both.line_accepted =
            dftracer::utils::yaml::DocumentTestAccess::is_line_backed(*lines);
        both.line_dump = full_dump(lines->root());
        if (!both.line_accepted) both.line_dump = both.tape_dump;
    } else {
        both.line_error = String(lines.error().message());
        both.line_invalid = line_spoke;
    }
    return both;
}

}  // namespace dftu_yaml_test

#endif  // DFTRACER_UTILS_TESTS_YAML_EQUAL_H
