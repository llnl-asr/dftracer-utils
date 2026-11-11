// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/yaml/document.h>
#include <dftracer/utils/yaml/document_test.h>
#include <dftracer/utils/yaml/line_index.h>
#include <dftracer/utils/yaml/records.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <yaml/yaml_dump.h>
#include <yaml/yaml_equal.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

using namespace dftracer::utils;
using dftu_yaml_test::parse_both;
using yaml::Document;
using yaml::DocumentTestAccess;
using yaml::LineIndex;
using yaml::Records;
using yaml::RecordsOptions;
using yaml::Value;

namespace {

// The reasons of the line index and of the tape reader for the same text, as
// pairs with the numbers and the quoted names cut out, and how often each
// came up.
dstd::Map<dstd::String, std::size_t>& reason_log() {
    static dstd::Map<dstd::String, std::size_t> log;
    return log;
}

dstd::String shape_of(dstd::StringView reason) {
    dstd::String out;
    bool in_quote = false;
    for (char c : reason) {
        if (c == '\'') {
            in_quote = !in_quote;
            out += c;
        } else if (!in_quote && (c < '0' || c > '9')) {
            out += c;
        } else if (!in_quote && (out.empty() || out.back() != 'N')) {
            out += 'N';
        }
    }
    return out;
}

// rapidyaml checks in a debug build that no line holds a CR before other
// text, and aborts on one. The tests leave such a text out of a debug build.
bool has_inner_cr(dstd::StringView text) {
#ifndef NDEBUG
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\r') continue;
        std::size_t j = i;
        while (j < text.size() && text[j] == '\r') ++j;
        if (j < text.size() && text[j] != '\n') return true;
        i = j;
    }
#else
    (void)text;
#endif
    return false;
}

// Each error the line index reports, by the start of its reason, with an
// example. The tape reader names the line of the cause for a duplicate key
// that it finds too. For every other class it stops on the line after the
// cause, or further on (an unterminated quote is found at the end of the
// file), so its line is the same or later, and the test allows that.
struct ErrorClass {
    const char* reason;
    const char* example;
    bool is_exact;
};

const ErrorClass ERROR_CLASSES[] = {
    {"duplicate key", "a: 1\na: 2\n", true},
    {"a complete value cannot go on in a deeper line", "a: \"x\"\n  y\n",
     false},
    {"a mapping key cannot follow sequence items", "- a\nb: 1\n", false},
    {"a sequence item cannot follow mapping keys", "a: 1\n- b\n", false},
    {"escape ", "a: \"\\xZZ\"\n", false},
    {"unknown escape character", "a: \"\\q\"\n", false},
    {"indentation does not match any open block", "a:\n    b: 1\n  c: 2\n",
     false},
    {"mapping values are not allowed in a multi-line plain scalar",
     "a: b\n  c: d\n", false},
    {"mapping values are not allowed in this context", "a: b: c\n", false},
    {"unexpected text after a quoted scalar", "a: \"x\" y\n", false},
    {"unterminated quoted scalar", "a: \"x\nb: 1\n", false},
};

const ErrorClass* error_class_of(dstd::StringView reason) {
    for (const ErrorClass& c : ERROR_CLASSES)
        if (reason.substr(0, std::strlen(c.reason)) == c.reason) return &c;
    return nullptr;
}

// The line index must give the tape result, or decline, or find an error that
// the tape reader finds too, on the line that ERROR_CLASSES says.
// Returns whether it took the text.
bool same_as_tape(dstd::StringView text) {
    if (has_inner_cr(text)) return false;
    const auto both = parse_both(text);
    if (both.line_invalid) {
        if (both.tape_ok) {
            FAIL_CHECK(
                "the line index fails a text that the tape reads\nline: ",
                dstd::String(both.line_error.data(), both.line_error.size()),
                "\ntext: ", dstd::String(text.data(), text.size()));
            return false;
        }
        const std::uint64_t ours = dftu_yaml_test::error_line(both.line_error);
        const std::uint64_t theirs =
            dftu_yaml_test::error_line(both.tape_error);
        const dstd::StringView reason =
            dftu_yaml_test::error_reason(both.line_error);
        const ErrorClass* error_class = error_class_of(reason);
        if (error_class == nullptr) {
            FAIL_CHECK("an error class that the test does not list: ",
                       dstd::String(reason.data(), reason.size()));
        }
        const bool is_same_error =
            error_class != nullptr && error_class->is_exact &&
            dftu_yaml_test::error_reason(both.tape_error)
                    .substr(0, std::strlen(error_class->reason)) ==
                error_class->reason;
        if (theirs < ours || (is_same_error && theirs != ours)) {
            FAIL_CHECK(
                "the error lines differ\nline: ",
                dstd::String(both.line_error.data(), both.line_error.size()),
                "\ntape: ",
                dstd::String(both.tape_error.data(), both.tape_error.size()),
                "\ntext: ", dstd::String(text.data(), text.size()));
        }
        ++reason_log()[shape_of(reason) + "  <=>  " +
                       shape_of(dftu_yaml_test::error_reason(both.tape_error)) +
                       (theirs == ours       ? "  [same line]"
                        : theirs == ours + 1 ? "  [tape +1 line]"
                                             : "  [tape later]")];
        return false;
    }
    if (both.line_dump != both.tape_dump) {
        std::size_t at = 0;
        while (at < both.line_dump.size() && at < both.tape_dump.size() &&
               both.line_dump[at] == both.tape_dump[at])
            ++at;
        const std::size_t from = at > 60 ? at - 60 : 0;
        FAIL_CHECK(
            "text: ", dstd::String(text.data(), text.size()), "\nline: ...",
            dstd::String(
                both.line_dump.data() + from,
                std::min<std::size_t>(160, both.line_dump.size() - from)),
            "\ntape: ...",
            dstd::String(
                both.tape_dump.data() + from,
                std::min<std::size_t>(160, both.tape_dump.size() - from)),
            "\ntape error: ",
            dstd::String(both.tape_error.data(), both.tape_error.size()));
    }
    return both.line_accepted;
}

struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    std::size_t below(std::size_t n) {
        return static_cast<std::size_t>(next() % n);
    }
    bool chance(std::size_t percent) { return below(100) < percent; }
};

const char* const WORDS[] = {"alpha",
                             "beta",
                             "gamma",
                             "x",
                             "key",
                             "a b",
                             "k1",
                             "k2",
                             "k3",
                             "-5",
                             "a:b",
                             "a#b",
                             "1,2",
                             "a[0]",
                             "it's",
                             "q\"q",
                             "42",
                             "1.5",
                             "true",
                             "No",
                             "ON",
                             "~",
                             "null",
                             "Null",
                             "0x1F",
                             "1e3",
                             ".inf",
                             "-.NaN",
                             "+7",
                             "9007199254740993",
                             "18446744073709551615",
                             "-9223372036854775808"};
const char* const KEYS[] = {"a",   "b",  "c",    "id", "name", "k 1", "x-y",
                            "z_z", "10", "true", "A",  "ab",   "abc", "a.b"};

class Gen {
   public:
    Gen(std::uint64_t seed) : rng_{seed * 0x9E3779B97F4A7C15ull + 12345} {}

    dstd::String text() {
        out_.clear();
        crlf_ = rng_.chance(8);
        step_ = 1 + rng_.below(4);
        if (rng_.chance(3)) out_ += "\xEF\xBB\xBF";
        std::size_t docs = rng_.chance(10) ? 1 + rng_.below(3) : 0;
        if (docs == 0) {
            block(0, rng_.chance(40));
        } else {
            for (std::size_t d = 0; d < docs; ++d) {
                line(0, "---");
                block(0, rng_.chance(40));
                if (rng_.chance(20)) line(0, "...");
            }
        }
        dstd::String text;
        for (char c : out_) {
            if (c == '\n' && crlf_) text += '\r';
            text += c;
        }
        return text;
    }

   private:
    void line(std::size_t indent, dstd::StringView body) {
        noise(indent);
        out_ += dstd::String(indent, ' ');
        out_ += body;
        if (rng_.chance(5)) out_ += "  ";
        out_ += '\n';
    }

    void noise(std::size_t indent) {
        if (rng_.chance(6)) out_ += "\n";
        if (rng_.chance(6)) {
            out_ += dstd::String(rng_.below(indent + 3), ' ');
            out_ += "# note: a comment\n";
        }
    }

    dstd::String base_key() {
        dstd::String k = KEYS[rng_.below(sizeof(KEYS) / sizeof(KEYS[0]))];
        if (rng_.chance(30)) k += dstd::fmt<"{}">(rng_.below(40));
        return k;
    }

    dstd::String decorate(const dstd::String& k) {
        if (rng_.chance(8)) return "\"" + k + "\"";
        if (rng_.chance(5)) return "'" + k + "'";
        return k;
    }

    dstd::String key() { return decorate(base_key()); }

    // A key that the map has not got, most of the time.
    dstd::String fresh_key(dstd::Vector<dstd::String>& used) {
        dstd::String base = base_key();
        for (int tries = 0; tries < 8 && !rng_.chance(3); ++tries) {
            bool seen = false;
            for (const auto& u : used) seen = seen || u == base;
            if (!seen) break;
            base = base_key();
        }
        used.push_back(base);
        return decorate(base);
    }

    dstd::String scalar() {
        const std::size_t kind = rng_.below(10);
        dstd::String s;
        if (kind < 5) {
            s = WORDS[rng_.below(sizeof(WORDS) / sizeof(WORDS[0]))];
        } else if (kind == 5) {
            s = "\"" + escaped_dq() + "\"";
        } else if (kind == 6) {
            s = "'" + escaped_sq() + "'";
        } else if (kind == 7) {
            s = dstd::fmt<"{}">(static_cast<std::int64_t>(rng_.next() >> 20) -
                                (1ll << 42));
        } else if (kind == 8) {
            s = rng_.chance(50) ? "{}" : "[]";
        } else {
            s = "";
        }
        if (!s.empty() && rng_.chance(10)) s += " # trailing";
        return s;
    }

    dstd::String escaped_dq() {
        static const char* const PARTS[] = {
            "a",       "b c",     "\\n",  "\\t", "\\\\", "\\\"",        "\\x41",
            "\\u00e9", "\\u2028", "it's", ": ",  " #x",  "\\/",         "\\0",
            "\\e",     "\\_",     "\\N",  "\\L", "\\P",  "\\U0001F600", "{}"};
        dstd::String s;
        const std::size_t n = rng_.below(5);
        for (std::size_t i = 0; i < n; ++i)
            s += PARTS[rng_.below(sizeof(PARTS) / sizeof(PARTS[0]))];
        return s;
    }

    dstd::String escaped_sq() {
        static const char* const PARTS[] = {"a",   "b c", "''",  "\"",
                                            "\\n", ": ",  " #x", "[]"};
        dstd::String s;
        const std::size_t n = rng_.below(5);
        for (std::size_t i = 0; i < n; ++i)
            s += PARTS[rng_.below(sizeof(PARTS) / sizeof(PARTS[0]))];
        return s;
    }

    // One map or sequence at `indent`.
    void block(std::size_t indent, bool is_seq, std::size_t depth = 0) {
        const std::size_t n = 1 + rng_.below(depth == 0 ? 6 : 4);
        dstd::Vector<dstd::String> used;
        for (std::size_t i = 0; i < n; ++i) {
            if (is_seq) {
                item(indent, depth);
            } else {
                entry(indent, fresh_key(used), depth, dstd::String());
            }
        }
    }

    void entry(std::size_t indent, const dstd::String& k, std::size_t depth,
               dstd::String prefix) {
        const std::size_t kind = rng_.below(10);
        const std::size_t child = indent + step_ + (rng_.chance(10) ? 1 : 0);
        if (kind < 6 || depth >= 4) {
            noise(indent);
            const dstd::String s = scalar();
            out_ += dstd::String(indent, ' ') + prefix + k + ":" +
                    (s.empty() ? dstd::String() : " " + s);
            if (s.empty() && rng_.chance(10)) out_ += " # c";
            out_ += '\n';
        } else {
            line(indent, prefix + k + ":");
            block(kind == 9 && rng_.chance(40) ? indent : child, kind == 9,
                  depth + 1);
        }
    }

    void item(std::size_t indent, std::size_t depth) {
        const std::size_t kind = rng_.below(10);
        const std::size_t child = indent + step_ + (rng_.chance(10) ? 1 : 0);
        if (kind < 4 || depth >= 4) {
            const dstd::String s = scalar();
            line(indent, s.empty() ? "-" : "- " + s);
        } else if (kind < 8) {
            // A compact map: `- k: v` and its next members.
            noise(indent);
            const std::size_t gap = 1 + (rng_.chance(10) ? rng_.below(3) : 0);
            const std::size_t col = indent + 1 + gap;
            dstd::String body =
                dstd::String(indent, ' ') + "-" + dstd::String(gap, ' ');
            const std::size_t members = 1 + rng_.below(3);
            dstd::Vector<dstd::String> used;
            for (std::size_t m = 0; m < members; ++m) {
                if (m == 0) {
                    out_ += body;
                    entry_at_col(col, fresh_key(used), depth);
                } else {
                    entry_at_col(col, fresh_key(used), depth, true);
                }
            }
        } else if (kind == 8) {
            line(indent, "-");
            block(child, rng_.chance(50), depth + 1);
        } else {
            line(indent, "- " + dstd::String(rng_.chance(50) ? "- a" : "[a]"));
        }
    }

    // The rest of a map entry whose indent is already written (first) or not.
    void entry_at_col(std::size_t col, const dstd::String& k, std::size_t depth,
                      bool indent = false) {
        if (indent) out_ += dstd::String(col, ' ');
        const std::size_t kind = rng_.below(10);
        if (kind < 7 || depth >= 3) {
            const dstd::String s = scalar();
            out_ += k + ":" + (s.empty() ? dstd::String() : " " + s) + "\n";
        } else {
            out_ += k + ":\n";
            block(kind == 9 && rng_.chance(40) ? col : col + step_, kind == 9,
                  depth + 1);
        }
    }

    Rng rng_;
    dstd::String out_;
    bool crlf_ = false;
    std::size_t step_ = 2;
};

dstd::String mutate(dstd::String text, Rng& rng) {
    static const char ALPHABET[] = ": -#'\"\\[]{}&*!|>%@`\t\n\r,?~abc19";
    const std::size_t n = 1 + rng.below(3);
    for (std::size_t i = 0; i < n && !text.empty(); ++i) {
        const std::size_t at = rng.below(text.size());
        const std::size_t op = rng.below(5);
        const char c = ALPHABET[rng.below(sizeof(ALPHABET) - 1)];
        if (op == 0) {
            text.erase(at, 1);
        } else if (op == 1) {
            text.insert(at, 1, c);
        } else if (op == 2) {
            text[at] = c;
        } else if (op == 3) {
            text.insert(at, dstd::String(1 + rng.below(3), ' '));
        } else {
            const std::size_t eol = text.find('\n', at);
            if (eol != dstd::String::npos) text.erase(eol, 1);
        }
    }
    return text;
}

// Changes the shape of the text a line at a time: the indent of a line, a
// repeated line, a dropped line, two swapped lines.
dstd::String mutate_lines(const dstd::String& text, Rng& rng) {
    dstd::Vector<dstd::String> lines;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t eol = text.find('\n', at);
        if (eol == dstd::String::npos) eol = text.size() - 1;
        lines.push_back(text.substr(at, eol - at + 1));
        at = eol + 1;
    }
    if (lines.empty()) return text;
    const std::size_t n = 1 + rng.below(3);
    for (std::size_t i = 0; i < n && !lines.empty(); ++i) {
        const std::size_t k = rng.below(lines.size());
        const std::size_t op = rng.below(5);
        if (op == 0) {
            lines[k].insert(0, dstd::String(1 + rng.below(3), ' '));
        } else if (op == 1) {
            const std::size_t gone = std::min<std::size_t>(
                1 + rng.below(3), lines[k].find_first_not_of(' '));
            if (gone != dstd::String::npos) lines[k].erase(0, gone);
        } else if (op == 2) {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(k),
                         lines[k]);
        } else if (op == 3) {
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(k));
        } else if (k + 1 < lines.size()) {
            std::swap(lines[k], lines[k + 1]);
        }
    }
    dstd::String out;
    for (const auto& line : lines) out += line;
    return out;
}

}  // namespace

TEST_CASE("line index: accepted subset reads as the tape reader does") {
    const char* const ACCEPTED[] = {
        "a: 1\nb: 2\n",
        "a: 1\nb: 2",
        "a: 1\r\nb: 2\r\n",
        "a:\n  b: 1\n  c:\n    - x\n    - y\n",
        "- a\n- b: 1\n  c: 2\n-\n",
        "- a: 1\n  b: 2\n- c: 3\n",
        "-   a: 1\n    b: 2\n",
        "a: 'it''s'\nb: \"x\\ty\\u00e9\"\nc: \"\"\nd: ''\n",
        "\"a b\": 1\n'c': 2\n\"\": 3\n",
        "a: 1 # c\n# full\n\nb: 2  # d\n",
        "foo:\n- 42\nbar:\n  - 44\n",
        "one:\n- 2\n- 3\nfour: 5\n",
        "- k:\n  - a\n  - b\n  z: 1\n- k:\n  - c\n",
        "a:\n- x: 1\n  y:\n  - q\nb: 2\n",
        "a: {}\nb: []\nc:\n",
        "- {}\n- []\n- ~\n- null\n-\n",
        "a: ~\nb: null\nc: Null\nd: NULL\ne:\n",
        "a: -5\nb: a:b\nc: a#b\nd: 1,2\ne: it's\nf: q\"q\n",
        "---\na: 1\n---\nb: 2\n",
        "---\na: 1\n...\n---\nb: 2\n...\n",
        "a:\n  - b: 1\n    c:\n      d: 2\n  - e\n",
        "k: v   \n",
        "k:    v\n",
        "a : 1\n",
        "a: \"q\" # c\n",
        "  # leading comment\na: 1\n",
        "a: 18446744073709551615\nb: -9223372036854775808\nc: 1e3\nd: .inf\n",
    };
    for (const char* text : ACCEPTED) {
        INFO("text: ", dstd::String(text));
        CHECK(same_as_tape(text));
    }
}

TEST_CASE("line index: declined text still reads as the tape reader does") {
    const char* const DECLINED[] = {
        "a: 1\na: 2\n",
        "a: 1\n  b\n",
        "a: b: c\n",
        "a: |\n  text\n",
        "a: >\n  text\n",
        "a: &x 1\nb: *x\n",
        "a: !!str 1\n",
        "? a\n: b\n",
        "%YAML 1.2\n---\na: 1\n",
        "a: [1, 2]\n",
        "a: {b: 1}\n",
        "a: 'multi\n  line'\n",
        "a: \"multi\n  line\"\n",
        "a: 1\n\tb: 2\n",
        "<<: {a: 1}\n",
        "~: 1\n",
        "null: 1\n",
        "   a: 1\n",
        "a: 1\n b: 2\n",
        "a:\n    b: 1\n  c: 2\n",
        "just text\n",
        "---\n---\na: 1\n",
        "a: 1\n...\n",
        "a: 1\n---\n",
        "\n",
        "# only a comment\n",
        "",
        "a: 1\xEF\xBB\xBF\n",
        "a: \"\\q\"\n",
        "a: \"\\x4\"\n",
        "- - a\n",
        "a: 1\r",
    };
    for (const char* text : DECLINED) {
        INFO("text: ", text);
        same_as_tape(text);
    }
    // The ones that must be declined.
    for (const char* text :
         {"a: 1\na: 2\n", "a: 1\n  b\n", "a: b: c\n", "a: |\n  text\n",
          "a: &x 1\nb: *x\n", "~: 1\n", "<<: {a: 1}\n"}) {
        const auto both = parse_both(text);
        CHECK_FALSE(both.line_accepted);
    }
}

TEST_CASE("line index: a leading byte order mark is no part of the text") {
    const char* const TEXTS[] = {
        "\xEF\xBB\xBF"
        "a: 1\nb: 2\n",
        "\xEF\xBB\xBF"
        "- a\n- b: 1\n  c: 2\n",
        "\xEF\xBB\xBF"
        "---\na: 1\n",
        "\xEF\xBB\xBF"
        "# c\na: 1\n",
        "\xEF\xBB\xBF"
        "\"a\": \"x\"\nb:\n  - 1\n",
        "\xEF\xBB\xBF"
        "a: 1\na: 2\n",
        "\xEF\xBB\xBF"
        "x-y: 1\nz_z26:\n\n- 10: +7\n  x-y: 69\n  name: No- \"\\P #x: "
        "\"\n\"abc\":\n",
        "\xEF\xBB\xBF"
        "\n",
        "\xEF\xBB\xBF",
    };
    for (const char* text : TEXTS) {
        INFO("text: ", text);
        same_as_tape(text);
    }
    const auto both = parse_both(
        "\xEF\xBB\xBF"
        "a: 1\nb: 2\n");
    CHECK(both.line_accepted);
    CHECK(both.line_dump == parse_both("a: 1\nb: 2\n").line_dump);
    // A byte order mark inside the text stays a character of the key.
    const auto inside = parse_both(
        "a: 1\n\xEF\xBB\xBF"
        "b: 2\n");
    CHECK_FALSE(inside.line_accepted);
}

TEST_CASE(
    "line index: a column past the 16-bit limit declines or errors as the tape "
    "does") {
    const dstd::String wide(70000, ' ');
    const dstd::String edge(65535, ' ');
    const dstd::String drop(4465, ' ');
    const dstd::String texts[] = {
        dstd::String("-") + wide + "a: 1\n" + drop + "b: 2\n",
        dstd::String("-") + edge + "a: 1\nb: 2\n",
        dstd::String("- a: 1\n-") + edge + "a: 1\nb: 2\n",
        edge + "a: 1\n",
        dstd::String("a: 1\n") + edge + "\nb: 2\n",
        dstd::String("a: 1\n") + dstd::String(65534, ' ') + "\nb: 2\n",
    };
    for (const auto& text : texts) {
        same_as_tape(text);
        CHECK(parse_both(text).line_accepted ==
              (text.find("a: 1\n" + dstd::String(65534, ' ') + "\n") == 0));
    }
}

TEST_CASE("line index: every escape reads as the tape reader does") {
    for (const char* escape :
         {"\\\\",    "\\\"",    "\\/",         "\\n",         "\\t",
          "\\r",     "\\b",     "\\f",         "\\a",         "\\e",
          "\\v",     "\\0",     "\\_",         "\\N",         "\\L",
          "\\P",     "\\x41",   "\\xe9",       "\\u00e9",     "\\u20ac",
          "\\u2028", "\\uD83D", "\\U0001F600", "\\U00110000", "\\ ",
          "\\q",     "\\x00",   "\\u0000"}) {
        dstd::String text = "a: \"x";
        text += escape;
        text += "y\"\n";
        INFO("escape: ", escape);
        same_as_tape(text);
        dstd::String key = "\"k";
        key += escape;
        key += "\": 1\nk: 2\n";
        same_as_tape(key);
    }
}

TEST_CASE("line index: a differential fuzz of generated and mutated text") {
    std::size_t accepted = 0;
    std::size_t pure_accepted = 0;
    std::size_t SEEDS = 4000;
    if (const char* more = std::getenv("YAML_FUZZ_SEEDS"))
        SEEDS = std::strtoul(more, nullptr, 10);
    for (std::size_t seed = 1; seed <= SEEDS; ++seed) {
        Gen gen(seed);
        const dstd::String text = gen.text();
        const bool took = same_as_tape(text);
        pure_accepted += took ? 1 : 0;
        Rng rng{seed * 7919 + 1};
        const dstd::String mutated = mutate(text, rng);
        accepted += same_as_tape(mutated) ? 1 : 0;
        same_as_tape(mutate_lines(text, rng));
    }
    MESSAGE("accepted: ", pure_accepted, " of ", SEEDS, " generated, ",
            accepted, " of ", SEEDS, " mutated");
    CHECK(pure_accepted > SEEDS / 3);
    for (const auto& [pair, count] : reason_log())
        MESSAGE("error reasons ", count, "x: ", pair);
}

TEST_CASE("line index: lines and quotes across 64-byte blocks") {
    for (std::size_t pad = 0; pad < 200; ++pad) {
        const dstd::String padding(pad, 'p');
        const dstd::String texts[] = {
            "# " + padding + "\na: 'q''q'\nb: \"x\\ny\"\nc: 1\n",
            "a: " + padding + "\nb: 'q''q'\nc: \"x\\ny\" # trailing\n",
            "k" + padding + ": v\n- x\n",
            "- " + padding + ": 1\n  b: 2\n",
            "a: 1\r\nb: \"" + padding + "\"\r\nc: '" + padding + "'\r\n",
            dstd::String(pad, '\n') + "a: 1\n",
            dstd::String(pad, ' ') + "# c\na: 1\n",
            "a:\n" + dstd::String(pad % 8, ' ') + " b: " + padding + "\n",
            "k: " + padding + ":\n",
            "k: " + padding + " # c\n",
        };
        for (const auto& text : texts) same_as_tape(text);
    }
    for (std::size_t pad = 0; pad < 64; ++pad) {
        dstd::String text = dstd::String(pad, ' ');
        text += "# x\n";
        text += "key: \"value with : and # inside\"\nother: 'it''s'\n";
        const auto both = parse_both(text);
        CHECK(both.line_dump == both.tape_dump);
        CHECK(both.line_accepted);
    }
}

TEST_CASE("line index: the scalar Highway target builds the same index") {
    dstd::String text;
    for (std::size_t i = 0; i < 3000; ++i) {
        Gen gen(i + 1);
        text += "- id: " + dstd::fmt<"{}">(i) + "\n  name: \"n" +
                dstd::fmt<"{}">(i * 7) + "\"\n  list:\n    - a\n    - 'b''c'\n";
        if (i % 7 == 0) text += "  # comment\n\n";
    }
    LineIndex best;
    REQUIRE(best.build(text.data(), text.size()) ==
            yaml::BuildResult::Accepted);
    yaml::detail::set_scalar_line_masks_for_test(true);
    LineIndex scalar;
    const bool ok =
        scalar.build(text.data(), text.size()) == yaml::BuildResult::Accepted;
    yaml::detail::set_scalar_line_masks_for_test(false);
    REQUIRE(ok);
    REQUIRE(best.line_count == scalar.line_count);
    CHECK(std::memcmp(best.entries.data(), scalar.entries.data(),
                      (best.line_count + 1) * sizeof(yaml::LineEntry)) == 0);
    CHECK(std::memcmp(best.next.data(), scalar.next.data(),
                      (best.line_count + 1) * sizeof(std::uint32_t)) == 0);
    CHECK(best.escapes.size() == scalar.escapes.size());
}

TEST_CASE("line index: the plain-line path and the byte classifier agree") {
    std::size_t built = 0;
    for (std::size_t seed = 1; seed <= 20000; ++seed) {
        Gen gen(seed);
        Rng rng{seed * 31 + 7};
        dstd::String text = gen.text();
        if (seed % 2 == 0) text = mutate(text, rng);
        LineIndex plain;
        const auto plain_ok = plain.build(text.data(), text.size());
        yaml::detail::set_slow_lines_for_test(true);
        LineIndex slow;
        const auto slow_ok = slow.build(text.data(), text.size());
        yaml::detail::set_slow_lines_for_test(false);
        REQUIRE(plain_ok == slow_ok);
        if (plain_ok != yaml::BuildResult::Accepted) continue;
        ++built;
        REQUIRE(plain.line_count == slow.line_count);
        CHECK(std::memcmp(plain.entries.data(), slow.entries.data(),
                          (plain.line_count + 1) * sizeof(yaml::LineEntry)) ==
              0);
        CHECK(std::memcmp(plain.next.data(), slow.next.data(),
                          (plain.line_count + 1) * sizeof(std::uint32_t)) == 0);
    }
    CHECK(built > 5000);
    // Lines of more than 65534 spaces, blank or not.
    for (const std::size_t spaces : {65533u, 65534u, 65535u, 65536u, 70000u}) {
        const dstd::String pad(spaces, ' ');
        const dstd::String texts[] = {
            "a: 1\n" + pad + "\nb: 2\n",
            "a: 1\n" + pad + "\n",
            pad + "\n",
            "- a: 1\n" + pad + "# c\n- b: 2\n",
        };
        for (const auto& text : texts) {
            LineIndex plain;
            const auto plain_ok = plain.build(text.data(), text.size());
            yaml::detail::set_slow_lines_for_test(true);
            LineIndex slow;
            const auto slow_ok = slow.build(text.data(), text.size());
            yaml::detail::set_slow_lines_for_test(false);
            CHECK(plain_ok == slow_ok);
            same_as_tape(text);
        }
    }
}

TEST_CASE("line index: each kind of error, with its message") {
    struct Case {
        const char* what;
        const char* text;
        const char* message;
    };
    const Case CASES[] = {
        {"duplicate key", "a: 1\nb: 2\na: 3\n",
         "n.yaml:3:1: duplicate key 'a'"},
        {"duplicate key, quoted", "\"a\": 1\n'a': 2\n",
         "n.yaml:2:1: duplicate key 'a'"},
        {"duplicate key in a compact map", "- a: 1\n  b: 2\n  a: 3\n",
         "n.yaml:3:3: duplicate key 'a'"},
        {"indent matches no level", "a:\n    b: 1\n  c: 2\n",
         "n.yaml:3:3: indentation does not match any open block"},
        {"item after keys", "a: 1\n- b\n",
         "n.yaml:2:1: a sequence item cannot follow mapping keys at the same "
         "indentation"},
        {"key after items", "- a\nb: 1\n",
         "n.yaml:2:1: a mapping key cannot follow sequence items at the same "
         "indentation"},
        {"unterminated double quote", "a: \"abc\n",
         "n.yaml:1:4: unterminated quoted scalar"},
        {"unterminated single quote", "a: 'abc\nb: 1\n",
         "n.yaml:1:4: unterminated quoted scalar"},
        {"text after a quoted value", "a: \"x\" y\n",
         "n.yaml:1:8: unexpected text after a quoted scalar"},
        {"unknown escape", "a: \"\\q\"\n",
         "n.yaml:1:5: unknown escape character 'q'"},
        {"short hex escape", "a: \"\\x4\"\n",
         "n.yaml:1:5: escape '\\x' needs 2 hex digits"},
        {"bad hex digit", "a: \"\\xZZ\"\n",
         "n.yaml:1:5: escape '\\x' has a character that is not a hex digit"},
        {"code point out of range", "a: \"\\U00110000\"\n",
         "n.yaml:1:5: escape is not a code point"},
        {"deeper line after a quoted value", "a: \"x\"\n  y\n",
         "n.yaml:2:3: a complete value cannot go on in a deeper line"},
        {"deeper line after an empty flow", "a: {}\n  y\n",
         "n.yaml:2:3: a complete value cannot go on in a deeper line"},
        {"key in a plain value that goes on", "a: b\n  c: d\n",
         "n.yaml:2:3: mapping values are not allowed in a multi-line plain "
         "scalar"},
        {"colon in a plain value", "a: b: c\n",
         "n.yaml:1:5: mapping values are not allowed in this context"},
    };
    for (const Case& c : CASES) {
        INFO("kind: ", dstd::String(c.what));
        auto line = Document::parse(dstd::String(c.text), "n.yaml");
        REQUIRE_FALSE(line.has_value());
        {
            yaml::LineIndex probe;
            CHECK(probe.build(c.text, std::strlen(c.text)) ==
                  yaml::BuildResult::Invalid);
        }
        CHECK(dstd::String(line.error().message()) == dstd::String(c.message));
        yaml::detail::set_force_tape_for_test(true);
        auto tape = Document::parse(dstd::String(c.text), "n.yaml");
        yaml::detail::set_force_tape_for_test(false);
        REQUIRE_FALSE(tape.has_value());
        MESSAGE("error kind '", dstd::String(c.what),
                "'\n  line index: ", dstd::String(line.error().message()),
                "\n  tape reader: ", dstd::String(tape.error().message()));
    }
}

TEST_CASE("line index: an error in Records names the line of the whole text") {
    for (std::size_t chunk :
         {std::size_t{1}, std::size_t{64}, std::size_t{1} << 20}) {
        dstd::String text;
        for (std::size_t i = 0; i < 50; ++i)
            text += "- id: " + dstd::fmt<"{}">(i) + "\n  name: \"n\\t\"\n";
        // The line of the whole text that holds the first fault is 101.
        text += "- id: 50\n  id: 51\n- id: 52\n";
        std::size_t seen_by_line = 0;
        for (int pass = 0; pass < 2; ++pass) {
            yaml::detail::set_force_tape_for_test(pass == 1);
            RecordsOptions options;
            options.chunk_bytes = chunk;
            Records records = Records::from_text(text, "r.yaml", options);
            std::size_t seen = 0;
            dstd::String error;
            for (;;) {
                auto next = records.next();
                if (!next.has_value()) {
                    error = dstd::String(next.error().message());
                    break;
                }
                if (!next->has_value()) break;
                ++seen;
            }
            REQUIRE_FALSE(error.empty());
            if (pass == 0) seen_by_line = seen;
            CHECK(seen == seen_by_line);
            if (pass == 0)
                CHECK(dstd::String(error.data(), error.size()) ==
                      "r.yaml:102:3: duplicate key 'id'");
            else
                CHECK(dftu_yaml_test::error_line(error) >= 102);
            MESSAGE(
                "chunk ", chunk,
                (pass == 0 ? " bytes, line index: " : " bytes, tape reader: "),
                dstd::String(error.data(), error.size()));
        }
        yaml::detail::set_force_tape_for_test(false);
    }
}

TEST_CASE("line index: records from a sequence and from documents") {
    dstd::String seq_text;
    dstd::String docs_text;
    for (std::size_t i = 0; i < 400; ++i) {
        seq_text +=
            "- id: " + dstd::fmt<"{}">(i) + "\n  tag: 'x'\n  n:\n    a: 1\n";
        docs_text += "---\nid: " + dstd::fmt<"{}">(i) + "\nname: \"d\\t" +
                     dstd::fmt<"{}">(i) + "\"\n";
    }
    const dstd::String seq = seq_text;
    const dstd::String docs = docs_text;
    const dstd::String seq_bom = dstd::String("\xEF\xBB\xBF") + seq_text;
    const dstd::String docs_bom = dstd::String("\xEF\xBB\xBF") + docs_text;
    for (const dstd::String* text : {&seq, &docs, &seq_bom, &docs_bom}) {
        for (std::size_t chunk : {std::size_t{1}, std::size_t{64},
                                  std::size_t{1000}, std::size_t{1} << 20}) {
            dstd::Vector<dstd::String> both[2];
            for (int pass = 0; pass < 2; ++pass) {
                yaml::detail::set_force_tape_for_test(pass == 1);
                RecordsOptions options;
                options.chunk_bytes = chunk;
                Records records = Records::from_text(*text, "r.yaml", options);
                for (;;) {
                    auto next = records.next();
                    REQUIRE(next.has_value());
                    if (!next->has_value()) break;
                    both[pass].push_back(dftu_yaml_test::full_dump(**next));
                }
            }
            yaml::detail::set_force_tape_for_test(false);
            REQUIRE(both[0].size() > 0);
            REQUIRE(both[0].size() == both[1].size());
            for (std::size_t i = 0; i < both[0].size(); ++i)
                CHECK(both[0][i] == both[1][i]);
        }
    }
}

TEST_CASE("line index: records equal the tape for generated documents") {
    for (std::size_t seed = 1; seed <= 600; ++seed) {
        Gen gen(seed);
        dstd::String text = gen.text();
        if (seed % 2 == 0) {
            Rng rng{seed};
            text = mutate(text, rng);
        }
        if (has_inner_cr(text)) continue;
        dstd::String out[2];
        dstd::String error[2];
        for (int pass = 0; pass < 2; ++pass) {
            yaml::detail::set_force_tape_for_test(pass == 1);
            RecordsOptions options;
            options.chunk_bytes = 1 + seed % 50;
            Records records = Records::from_text(text, "r.yaml", options);
            for (;;) {
                auto next = records.next();
                if (!next.has_value()) {
                    error[pass] = dstd::String(next.error().message());
                    break;
                }
                if (!next->has_value()) break;
                out[pass] += dftu_yaml_test::full_dump(**next);
                out[pass] += '\n';
            }
        }
        yaml::detail::set_force_tape_for_test(false);
        CHECK_MESSAGE(out[0] == out[1], "text: ", text.c_str());
        CHECK(error[0].empty() == error[1].empty());
        if (!error[0].empty() && !error[1].empty())
            CHECK_MESSAGE(
                dftu_yaml_test::error_line(error[1]) >=
                    dftu_yaml_test::error_line(error[0]),
                "line: ", dstd::String(error[0].data(), error[0].size()),
                " tape: ", dstd::String(error[1].data(), error[1].size()), "\n",
                dstd::String(text.data(), text.size()));
    }
}

TEST_CASE("line index: every case of the YAML test suite") {
    std::size_t cases = 0;
    std::size_t tape_valid = 0;
    std::size_t accepted = 0;
    std::size_t error_cases = 0;
    std::size_t invalid = 0;
    auto walked = dstd::fs::walk(
        DFTRACER_UTILS_YAML_TEST_SUITE_DIR,
        [&](dstd::PathView path, dstd::fs::FileKind kind) {
            if (kind != dstd::fs::FileKind::File ||
                path.filename() != "in.yaml")
                return dstd::fs::WalkAction::Continue;
            const dstd::String name(path.str());
            auto text = dstd::fs::read_file(path);
            REQUIRE(text.has_value());
            const bool is_error_case =
                dstd::fs::exists(dstd::Path(path.parent()) / "error")
                    .value_or(false);
            const auto both = parse_both(*text);
            ++cases;
            error_cases += is_error_case ? 1 : 0;
            tape_valid += both.tape_ok ? 1 : 0;
            accepted += both.line_accepted ? 1 : 0;
            invalid += both.line_invalid ? 1 : 0;
            CHECK_MESSAGE(both.line_dump == both.tape_dump, name);
            if (!both.tape_ok) CHECK_MESSAGE(!both.line_accepted, name);
            if (is_error_case) CHECK_MESSAGE(!both.line_accepted, name);
            if (both.line_invalid && !both.tape_ok)
                CHECK_MESSAGE(dftu_yaml_test::error_line(both.tape_error) >=
                                  dftu_yaml_test::error_line(both.line_error),
                              name);
            return dstd::fs::WalkAction::Continue;
        });
    REQUIRE(walked.has_value());
    MESSAGE("suite cases: ", cases, ", error cases: ", error_cases,
            ", tape reads: ", tape_valid, ", line index accepts: ", accepted,
            ", line index finds an error: ", invalid);
    CHECK(cases > 300);
    for (const auto& [pair, count] : reason_log())
        MESSAGE("error reasons ", count, "x: ", pair);
}

TEST_CASE("line index: each listed error class has an example that gives it") {
    for (const ErrorClass& c : ERROR_CLASSES) {
        INFO("class: ", c.reason);
        const auto both = parse_both(c.example);
        REQUIRE(both.line_invalid);
        CHECK(error_class_of(dftu_yaml_test::error_reason(both.line_error)) ==
              &c);
        const std::uint64_t ours = dftu_yaml_test::error_line(both.line_error);
        const std::uint64_t theirs =
            dftu_yaml_test::error_line(both.tape_error);
        if (c.is_exact)
            CHECK(theirs == ours);
        else
            CHECK(theirs >= ours);
    }
}
