// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/yaml/document.h>
#include <dftracer/utils/yaml/records.h>
#include <doctest/doctest.h>
#include <yaml/yaml_tree.h>

#include <cstdint>
#include <cstdio>

using namespace dftracer::utils;
using dftu_yaml_test::Tree;
using yaml::Document;
using yaml::Records;
using yaml::Value;

namespace {

// The features that the reader refuses, and the words of their reason.
enum Feature : unsigned {
    Anchor = 1,
    Alias = 2,
    Tag = 4,
    ComplexKey = 8,
    Directive = 16,
    NullKey = 32,
};

bool has_word(dstd::StringView line, dstd::StringView word) {
    return line.find(word) != dstd::StringView::npos;
}

bool is_null_spelling(dstd::StringView text) {
    return text.empty() || text == "~" || text == "null" || text == "Null" ||
           text == "NULL";
}

// The refused features that the events of a case name: an anchor or a tag on a
// node, an alias, a collection or a null as a key.
unsigned features_of_events(dstd::StringView events) {
    struct Frame {
        bool is_map;
        std::size_t count;
    };
    dstd::Vector<Frame> stack;
    unsigned found = 0;
    const auto is_key_position = [&] {
        return !stack.empty() && stack.back().is_map &&
               stack.back().count % 2 == 0;
    };
    while (!events.empty()) {
        const std::size_t end = events.find('\n');
        const dstd::StringView line = events.substr(0, end);
        events = end == dstd::StringView::npos ? dstd::StringView{}
                                               : events.substr(end + 1);
        const dstd::StringView type = line.substr(0, 4);
        // The properties come after the type, before the text of a scalar.
        dstd::StringView rest =
            line.size() > 5 ? line.substr(5) : dstd::StringView{};
        dstd::StringView text;
        while (!rest.empty()) {
            const char lead = rest[0];
            if (lead == ':' || lead == '"' || lead == '\'' || lead == '|' ||
                lead == '>') {
                text = rest;
                break;
            }
            const std::size_t space = rest.find(' ');
            if (lead == '&') found |= Anchor;
            if (lead == '<') found |= Tag;
            rest = space == dstd::StringView::npos ? dstd::StringView{}
                                                   : rest.substr(space + 1);
        }
        if (type == "+MAP" || type == "+SEQ") {
            if (is_key_position()) found |= ComplexKey;
            stack.push_back({type == "+MAP", 0});
        } else if (type == "-MAP" || type == "-SEQ") {
            if (!stack.empty()) stack.pop_back();
            if (!stack.empty()) ++stack.back().count;
        } else if (type == "=ALI") {
            found |= Alias;
            if (!stack.empty()) ++stack.back().count;
        } else if (type == "=VAL") {
            if (is_key_position() && !text.empty() && text[0] == ':' &&
                is_null_spelling(text.substr(1)))
                found |= NullKey;
            if (!stack.empty()) ++stack.back().count;
        }
    }
    return found;
}

constexpr std::size_t FEATURE_COUNT = 6;
constexpr std::size_t NO_LINE = ~std::size_t{0};

std::size_t feature_index(unsigned feature) {
    std::size_t i = 0;
    while ((feature >> i) != 1) ++i;
    return i;
}

// Whether the text of a line has a property or an alias that starts with
// `lead` at the start of a word.
bool has_token(dstd::StringView line, char lead) {
    for (std::size_t i = 0; i + 1 < line.size(); ++i) {
        if (line[i] != lead) continue;
        const bool is_start = i == 0 || line[i - 1] == ' ' ||
                              line[i - 1] == '-' || line[i - 1] == '[' ||
                              line[i - 1] == '{' || line[i - 1] == ',' ||
                              line[i - 1] == ':';
        if (is_start && line[i + 1] != ' ' && line[i + 1] != '\r') return true;
    }
    return false;
}

// The refused features that the text of a case holds: a `?` key and a `%`
// directive, outside the scalars that hold such a line as text. `first_line`
// gets the first line of each feature found in the text, anchors, aliases and
// tags included.
unsigned features_of_text(dstd::StringView text,
                          std::size_t (&first_line)[FEATURE_COUNT]) {
    unsigned found = 0;
    std::size_t line_number = 0;
    const auto note = [&](unsigned feature) {
        found |= feature;
        std::size_t& at = first_line[feature_index(feature)];
        if (at == NO_LINE) at = line_number;
    };
    bool is_prefix = true;
    long block_indent = -1;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        dstd::StringView line = text.substr(0, end);
        text = end == dstd::StringView::npos ? dstd::StringView{}
                                             : text.substr(end + 1);
        ++line_number;
        std::size_t i = 0;
        while (i < line.size() && line[i] == ' ') ++i;
        const bool is_blank =
            line.find_first_not_of(" \t\r") == dstd::StringView::npos;
        if (block_indent >= 0) {
            if (is_blank || static_cast<long>(i) > block_indent) continue;
            block_indent = -1;
        }
        if (is_blank || (i < line.size() && line[i] == '#')) continue;
        // These only place a feature that the events found.
        const auto place = [&](unsigned feature, char lead) {
            std::size_t& at = first_line[feature_index(feature)];
            if (at == NO_LINE && has_token(line, lead)) at = line_number;
        };
        place(Anchor, '&');
        place(Alias, '*');
        place(Tag, '!');
        if (line.substr(0, 3) == "...") {
            is_prefix = true;
            continue;
        }
        if (line.substr(0, 3) == "---") {
            is_prefix = false;
            continue;
        }
        if (is_prefix && line[0] == '%') {
            note(Directive);
            continue;
        }
        is_prefix = false;
        std::size_t k = i;
        while (k < line.size() && (line[k] == ' ' || line[k] == '-')) ++k;
        if (k < line.size() && line[k] == '?' &&
            (k + 1 == line.size() || line[k + 1] == ' ' || line[k + 1] == '\r'))
            note(ComplexKey);
        // A block scalar header that ends the line.
        std::size_t e = line.size();
        while (e > 0 && (line[e - 1] == ' ' || line[e - 1] == '\r')) --e;
        std::size_t h = e;
        while (h > 0 && (line[h - 1] == '+' || line[h - 1] == '-' ||
                         (line[h - 1] >= '0' && line[h - 1] <= '9')))
            --h;
        if (h > 0 && (line[h - 1] == '|' || line[h - 1] == '>') &&
            (h - 1 == 0 || line[h - 2] == ' '))
            block_indent = static_cast<long>(i);
    }
    return found;
}

bool reason_matches(dstd::StringView message, unsigned features) {
    return ((features & Anchor) != 0 &&
            has_word(message, "anchors are not supported")) ||
           ((features & Alias) != 0 &&
            has_word(message, "aliases are not supported")) ||
           ((features & Tag) != 0 &&
            has_word(message, "tags are not supported")) ||
           ((features & ComplexKey) != 0 &&
            (has_word(message, "complex keys are not supported") ||
             has_word(message, "a collection cannot be a key"))) ||
           ((features & Directive) != 0 &&
            has_word(message, "directives are not supported")) ||
           ((features & NullKey) != 0 &&
            has_word(message, "a null key is not supported"));
}

dstd::String read_or_empty(const dstd::Path& path) {
    auto text = dstd::fs::read_file(path);
    return text ? *text : dstd::String();
}

struct Counts {
    std::size_t cases = 0;
    std::size_t equal = 0;
    std::size_t errors = 0;
    std::size_t refused = 0;
    std::size_t without_json = 0;
    std::size_t mismatches = 0;
    std::size_t refused_by[FEATURE_COUNT] = {};
};

// Parses a case and compares it with its JSON. Returns a text that says why not
// when it differs, and empty when it is as expected.
dstd::String check_case(const dstd::Path& dir, Counts& counts) {
    const dstd::String text = read_or_empty(dir / "in.yaml");
    const bool is_error_case = dstd::fs::exists(dir / "error").value_or(false);
    std::size_t first_line[FEATURE_COUNT];
    for (std::size_t& at : first_line) at = NO_LINE;
    const unsigned features =
        features_of_events(read_or_empty(dir / "test.event")) |
        features_of_text(text, first_line);
    // The error is the one of the first feature in the text; the features of
    // that line are all allowed.
    std::size_t earliest = NO_LINE;
    for (std::size_t f = 0; f < FEATURE_COUNT; ++f)
        if ((features >> f & 1) != 0 && first_line[f] < earliest)
            earliest = first_line[f];
    unsigned expected_features = 0;
    for (std::size_t f = 0; f < FEATURE_COUNT; ++f)
        if ((features >> f & 1) != 0 &&
            (earliest == NO_LINE || first_line[f] == earliest))
            expected_features |= 1u << f;
    ++counts.cases;
    auto doc = Document::parse(text, "suite");
    // Every document of the text, through Records, which a Document does not
    // read past the first.
    dstd::Vector<dstd::String> record_dumps;
    dstd::String records_error;
    bool records_ok = true;
    dstd::Vector<Tree> expected;
    bool has_json = false;
    if (!is_error_case && features == 0) {
        const dstd::String json = read_or_empty(dir / "in.json");
        if (dstd::fs::exists(dir / "in.json").value_or(false)) {
            has_json = true;
            if (!dftu_yaml_test::trees_of_json(json, expected))
                return "the JSON of the case is not readable";
        }
    }
    if (is_error_case) {
        ++counts.errors;
        if (doc.has_value()) {
            // A text that a later document spoils fails in Records only.
            Records records = Records::from_text(text, "suite");
            for (;;) {
                auto next = records.next();
                if (!next) {
                    records_ok = false;
                    break;
                }
                if (!next->has_value()) break;
            }
            if (records_ok) return "an error case parsed";
        }
        return "";
    }
    if (features != 0) {
        ++counts.refused;
        ++counts.refused_by[feature_index(expected_features &
                                          (~expected_features + 1))];
        if (doc.has_value()) {
            Records records = Records::from_text(text, "suite");
            bool failed = false;
            for (;;) {
                auto next = records.next();
                if (!next) {
                    failed = true;
                    records_error = dstd::String(next.error().message());
                    break;
                }
                if (!next->has_value()) break;
            }
            if (!failed) return "a refused feature parsed";
            if (!reason_matches(records_error, expected_features))
                return "refused with another reason: " + records_error;
            return "";
        }
        const dstd::String message(doc.error().message());
        if (!reason_matches(message, expected_features))
            return "refused with another reason: " + message;
        return "";
    }
    if (!doc.has_value())
        return "a valid case failed: " + dstd::String(doc.error().message());
    if (!has_json) {
        ++counts.without_json;
        return "";
    }
    if (expected.size() != 1) {
        Records records = Records::from_text(text, "suite");
        std::size_t index = 0;
        for (;;) {
            auto next = records.next();
            if (!next)
                return "records failed: " +
                       dstd::String(next.error().message());
            if (!next->has_value()) break;
            // A record is valid until the next call, so it is compared at
            // once.
            dstd::String why;
            if (index >= expected.size())
                return "more records than JSON values";
            if (!dftu_yaml_test::matches(expected[index], **next, why))
                return "record " + dstd::fmt<"{}">(index) + ": " + why;
            ++index;
        }
        if (index != expected.size())
            return dstd::fmt<"{} records, {} JSON values">(index,
                                                           expected.size());
    }
    if (!expected.empty()) {
        dstd::String why;
        if (!dftu_yaml_test::matches(expected[0], doc->root(), why))
            return "the root of the document: " + why;
    }
    ++counts.equal;
    return "";
}

}  // namespace

TEST_CASE("yaml suite: every case reads as its JSON, fails or is refused") {
    Counts counts;
    auto walked =
        dstd::fs::walk(DFTRACER_UTILS_YAML_TEST_SUITE_DIR,
                       [&](dstd::PathView path, dstd::fs::FileKind kind) {
                           if (kind != dstd::fs::FileKind::File ||
                               path.filename() != "in.yaml")
                               return dstd::fs::WalkAction::Continue;
                           const dstd::Path dir(path.parent());
                           const dstd::String name(dir.str());
                           const dstd::String why = check_case(dir, counts);
                           if (!why.empty()) {
                               ++counts.mismatches;
                               std::fprintf(stderr, "MISMATCH %s: %s\n",
                                            name.c_str(), why.c_str());
                           }
                           return dstd::fs::WalkAction::Continue;
                       });
    REQUIRE(walked.has_value());
    MESSAGE("cases: ", counts.cases, ", equal to their JSON: ", counts.equal,
            ", errors as expected: ", counts.errors,
            ", refused as expected: ", counts.refused,
            ", valid with no JSON: ", counts.without_json,
            ", mismatches: ", counts.mismatches);
    MESSAGE("refused by first feature: anchors ", counts.refused_by[0],
            ", aliases ", counts.refused_by[1], ", tags ", counts.refused_by[2],
            ", complex keys ", counts.refused_by[3], ", directives ",
            counts.refused_by[4], ", null keys ", counts.refused_by[5]);
    CHECK(counts.cases > 300);
    CHECK(counts.mismatches == 0);
}
