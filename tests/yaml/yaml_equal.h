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

// Whether `text` has a shape on which rapidyaml 0.16.0 gives a result that the
// YAML 1.2.2 spec does not, and the line index follows the spec. Each shape is
// matched as it is declared; where it is hard to tell, the match errs toward
// false, so that the text is compared.
//
//  1. In a double quoted scalar, an escaped line break (`\` at the end of a
//     line) followed by an empty line: rapidyaml drops one of the line feeds
//     that the empty lines stand for.
//  2. A block scalar whose lines reach the end of a text that has no line
//     break at the end: rapidyaml adds the final line feed to a clipped one
//     and the spec does not.
//  3. A block scalar with no line, followed by a document marker: rapidyaml
//     takes the marker line for the text of the scalar.
// A tab before a line break in a plain scalar of several lines and a line of
// only a tab in a block scalar are declined by the line index, so they need no
// clause.
namespace known_difference {

inline bool is_blank_line(dftracer::utils::dstd::StringView line) {
    return line.find_first_not_of(" \t\r") ==
           dftracer::utils::dstd::StringView::npos;
}

inline std::size_t indent_of(dftracer::utils::dstd::StringView line) {
    const std::size_t at = line.find_first_not_of(' ');
    return at == dftracer::utils::dstd::StringView::npos ? line.size() : at;
}

inline bool is_marker(dftracer::utils::dstd::StringView line) {
    const auto lead = line.substr(0, 3);
    return (lead == "---" || lead == "...") &&
           (line.size() == 3 || line[3] == ' ' || line[3] == '\t' ||
            line[3] == '\r');
}

// Whether `line` has a block scalar header `|` or `>` with its indicators as
// the value indicator: after a `: `, a `- `, a `--- ` or at the start. Sets
// `root` when no key or dash is before it, so the body may have no indent.
inline bool is_block_header(dftracer::utils::dstd::StringView line,
                            bool& root) {
    using dftracer::utils::dstd::StringView;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] != '|' && line[i] != '>') continue;
        const StringView before = line.substr(0, i);
        const bool at_start = before.find_first_not_of(' ') == StringView::npos;
        const std::size_t word = before.find_last_not_of(' ');
        const bool after_dash_space = word != StringView::npos &&
                                      i > word + 1 &&
                                      (line[word] == ':' || line[word] == '-');
        const bool after_doc = word != StringView::npos && word + 1 < i &&
                               word >= 2 && line.substr(word - 2, 3) == "---" &&
                               (word == 2 || line[word - 3] == ' ');
        if (!at_start && !after_dash_space && !after_doc) continue;
        std::size_t j = i + 1;
        while (j < line.size() && (line[j] == '+' || line[j] == '-' ||
                                   (line[j] >= '0' && line[j] <= '9')))
            ++j;
        if (j < line.size() && line[j] != ' ' && line[j] != '\r') continue;
        root = at_start || after_doc;
        return true;
    }
    return false;
}

}  // namespace known_difference

inline bool is_known_difference(dftracer::utils::dstd::StringView text) {
    using dftracer::utils::dstd::StringView;
    struct Line {
        StringView text;
        std::size_t begin;
    };
    dftracer::utils::dstd::Vector<Line> lines;
    for (std::size_t at = text.substr(0, 3) == "\xEF\xBB\xBF" ? 3 : 0;
         at <= text.size();) {
        std::size_t eol = text.find('\n', at);
        if (eol == StringView::npos) eol = text.size();
        lines.push_back({text.substr(at, eol - at), at});
        at = eol + 1;
    }
    // Clause 1: `quote` follows the quotes that start a value.
    char quote = 0;
    for (std::size_t k = 0; k < lines.size(); ++k) {
        StringView line = lines[k].text;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        bool escaped_end = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            escaped_end = false;
            if (quote == '"') {
                if (line[i] == '\\') {
                    if (i + 1 == line.size())
                        escaped_end = true;
                    else
                        ++i;
                } else if (line[i] == '"') {
                    quote = 0;
                }
            } else if (quote == '\'') {
                if (line[i] == '\'') {
                    if (i + 1 < line.size() && line[i + 1] == '\'')
                        ++i;
                    else
                        quote = 0;
                }
            } else if (line[i] == '"' || line[i] == '\'') {
                const StringView before = line.substr(0, i);
                const std::size_t last = before.find_last_not_of(' ');
                if (last == StringView::npos ||
                    (before.back() == ' ' &&
                     (before[last] == ':' || before[last] == '-')))
                    quote = line[i];
            }
        }
        if (quote == '"' && escaped_end && k + 1 < lines.size() &&
            known_difference::is_blank_line(lines[k + 1].text))
            return true;
    }
    // Clauses 2 and 3 look at each block header.
    for (std::size_t header = 0; header < lines.size(); ++header) {
        bool root = false;
        if (!known_difference::is_block_header(lines[header].text, root))
            continue;
        const std::size_t base =
            known_difference::indent_of(lines[header].text);
        std::size_t k = header + 1;
        bool has_body = false;
        while (k < lines.size()) {
            const StringView line = lines[k].text;
            if (!known_difference::is_blank_line(line)) {
                if (root && known_difference::is_marker(line)) break;
                if (!root && known_difference::indent_of(line) <= base) break;
                has_body = true;
            }
            ++k;
        }
        // Clause 2: the body reaches the end of a text with no line break.
        if (k == lines.size() && !text.empty() && text.back() != '\n')
            return true;
        // Clause 3: no body, then a marker.
        if (!has_body && k < lines.size()) {
            if (known_difference::is_marker(lines[k].text)) return true;
        }
    }
    return false;
}

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
