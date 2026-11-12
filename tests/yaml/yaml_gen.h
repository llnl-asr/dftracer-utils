// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_YAML_GEN_H
#define DFTRACER_UTILS_TESTS_YAML_GEN_H

#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <yaml/yaml_tree.h>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace dftu_yaml_test {

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

// A random tree and its text. The tree has maps, sequences, strings and nulls.
// The text is made of every form that the reader supports and that the tree
// can say: block and flow collections nested in each other, plain, quoted,
// folded and block scalars, comments and line breaks inside flow collections.
class Gen {
   public:
    // How many times each form was written.
    struct Stats {
        std::size_t flow_collections = 0;
        std::size_t flow_multi_line = 0;
        std::size_t block_scalars = 0;
        std::size_t multi_line_plain = 0;
        std::size_t multi_line_quoted = 0;
        std::size_t gap_scalars = 0;
        std::size_t gap_flows = 0;
        std::size_t gap_block_scalars = 0;
        std::size_t gap_lines = 0;
        std::size_t unterminated_texts = 0;

        void add(const Stats& other) {
            flow_collections += other.flow_collections;
            flow_multi_line += other.flow_multi_line;
            block_scalars += other.block_scalars;
            multi_line_plain += other.multi_line_plain;
            multi_line_quoted += other.multi_line_quoted;
            gap_scalars += other.gap_scalars;
            gap_flows += other.gap_flows;
            gap_block_scalars += other.gap_block_scalars;
            gap_lines += other.gap_lines;
            unterminated_texts += other.unterminated_texts;
        }
    };

    const Stats& stats() const { return stats_; }

    explicit Gen(std::uint64_t seed)
        : rng_{seed * 0x9E3779B97F4A7C15ull + 88172645463325252ull} {
        for (int i = 0; i < 4; ++i) rng_.next();
    }

    Tree tree(int depth = 0) {
        Tree t;
        const std::size_t pick = rng_.below(100);
        if (depth >= 4 || pick < 35) {
            if (rng_.chance(12)) return t;
            t.kind = Tree::Kind::String;
            t.text = random_string();
            return t;
        }
        const std::size_t count = rng_.below(5);
        if (pick < 68) {
            t.kind = Tree::Kind::Map;
            for (std::size_t i = 0; i < count; ++i) {
                dftracer::utils::dstd::String key = key_string();
                bool repeated = false;
                for (const dftracer::utils::dstd::String& k : t.keys)
                    repeated |= k == key;
                if (repeated) continue;
                t.keys.push_back(std::move(key));
                t.items.push_back(tree(depth + 1));
            }
        } else {
            t.kind = Tree::Kind::Seq;
            for (std::size_t i = 0; i < count; ++i)
                t.items.push_back(tree(depth + 1));
        }
        return t;
    }

    // The text of `t` as the root of a document.
    dftracer::utils::dstd::String text(const Tree& t) {
        out_.clear();
        block_end_ = ~std::size_t{0};
        if (rng_.chance(15)) out_ += "---\n";
        if (t.kind == Tree::Kind::Seq || t.kind == Tree::Kind::Map) {
            if (rng_.chance(25)) {
                flow_node(t, 0, 1);
                out_ += '\n';
            } else {
                block_collection(t, 0, false);
            }
        } else if (t.kind == Tree::Kind::Null) {
            out_ += rng_.chance(50) ? "null\n" : "~\n";
        } else {
            root_scalar(t);
        }
        if (rng_.chance(10)) out_ += "...\n";
        // A text may end with no line feed, unless a block scalar ends in it,
        // which keeps its last line feed.
        if (out_.size() != block_end_ && out_.back() == '\n' &&
            rng_.chance(8)) {
            out_.pop_back();
            ++stats_.unterminated_texts;
        }
        return out_;
    }

   private:
    static bool is_alnum(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9');
    }

    dftracer::utils::dstd::String word() {
        static const char* const WORDS[] = {"alpha",
                                            "beta",
                                            "x",
                                            "key",
                                            "k1",
                                            "a-b",
                                            "a:b",
                                            "a#b",
                                            "42",
                                            "1.5",
                                            "true",
                                            "No",
                                            "0x1F",
                                            "1e3",
                                            "a/b",
                                            "it's",
                                            "q\"q",
                                            "a\\b",
                                            "~x",
                                            "-x",
                                            "?x",
                                            ":x",
                                            "x:",
                                            "a b",
                                            "a  b",
                                            " a",
                                            "b ",
                                            "<<",
                                            "%x",
                                            "&x",
                                            "*x",
                                            "!x",
                                            "@x",
                                            "`x",
                                            "a,b",
                                            "[a]",
                                            "{b}",
                                            "# c",
                                            "a #c",
                                            "\xC3\xA9",
                                            "\xE6\x97\xA5\xE6\x9C\xAC",
                                            "tab\there",
                                            "line\nbreak",
                                            "cr\rx",
                                            "nul\x01y",
                                            "a]b",
                                            "x{y",
                                            "-5",
                                            "@",
                                            "---",
                                            "...",
                                            "- a",
                                            "a: b",
                                            "\\",
                                            "'",
                                            "\"",
                                            "null",
                                            "~",
                                            "",
                                            " "};
        return WORDS[rng_.below(sizeof(WORDS) / sizeof(WORDS[0]))];
    }

    dftracer::utils::dstd::String random_string() {
        dftracer::utils::dstd::String s = word();
        if (rng_.chance(30)) s += " " + word();
        if (rng_.chance(5)) s += "\n";
        return s;
    }

    dftracer::utils::dstd::String key_string() {
        for (;;) {
            dftracer::utils::dstd::String s = word();
            if (rng_.chance(20)) s += word();
            if (!s.empty() && s != "~" && s != "null" &&
                s.find('\n') == dftracer::utils::dstd::String::npos)
                return s;
        }
    }

    static bool is_null_like(const dftracer::utils::dstd::String& s) {
        return s.empty() || s == "~" || s == "null" || s == "Null" ||
               s == "NULL";
    }

    // Whether `s` may be written plain in a block context, or in a flow one,
    // and read back as the same string: no indicator starts it, except `-`, `?`
    // and `:` before a character that is no space, and no `: ` or ` #` is in
    // it. In a flow collection `,[]{}` are no part of it either.
    static bool is_plain_safe(const dftracer::utils::dstd::String& s,
                              bool in_flow = false) {
        if (s.empty() || is_null_like(s)) return false;
        const auto is_flow_indicator = [](char c) {
            return c == ',' || c == '[' || c == ']' || c == '{' || c == '}';
        };
        // A marker line at column 0 ends the document, so no text may start
        // like one.
        if ((s.rfind("---", 0) == 0 || s.rfind("...", 0) == 0) &&
            (s.size() == 3 || s[3] == ' ' || s[3] == '\t'))
            return false;
        const char first = s.front();
        switch (first) {
            case '-':
            case '?':
            case ':':
                if (s.size() == 1 || s[1] == ' ' || s[1] == '\t') return false;
                if (in_flow && is_flow_indicator(s[1])) return false;
                break;
            case '#':
            case '&':
            case '*':
            case '!':
            case '|':
            case '>':
            case '\'':
            case '"':
            case '%':
            case '@':
            case '`':
            case ',':
            case '[':
            case ']':
            case '{':
            case '}':
            case ' ':
            case '\t':
                return false;
            default:
                break;
        }
        if (s.back() == ' ' || s.back() == '\t' || s.back() == ':')
            return false;
        for (std::size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            const auto u = static_cast<unsigned char>(c);
            if ((u < 0x20 && c != '\t') || u == 0x7F) return false;
            if (c == ' ' && s[i + 1] == ' ') return false;
            if (c == '\t' && (s[i - 1] == ' ' || s[i + 1] == ' ')) return false;
            if (c == ':' &&
                (s[i + 1] == ' ' || (in_flow && is_flow_indicator(s[i + 1]))))
                return false;
            if (c == '#' && (s[i - 1] == ' ' || s[i - 1] == '\t')) return false;
            if (in_flow && is_flow_indicator(c)) return false;
        }
        return true;
    }

    dftracer::utils::dstd::String single_quoted(
        const dftracer::utils::dstd::String& s) {
        dftracer::utils::dstd::String out = "'";
        for (char c : s) {
            out += c;
            if (c == '\'') out += '\'';
        }
        return out + "'";
    }

    // A double quoted scalar. When `fold` is a column, some spaces become a
    // line break and that many spaces of indentation.
    dftracer::utils::dstd::String double_quoted(
        const dftracer::utils::dstd::String& s, int fold) {
        dftracer::utils::dstd::String out = "\"";
        for (std::size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            const auto u = static_cast<unsigned char>(c);
            if (c == '"') {
                out += "\\\"";
            } else if (c == '\\') {
                out += "\\\\";
            } else if (c == '\n') {
                out += "\\n";
            } else if (c == '\t') {
                out += rng_.chance(50) ? "\\t" : "\t";
            } else if (c == '\r') {
                out += "\\r";
            } else if (u < 0x20 || u == 0x7F) {
                out += "\\x";
                out += "0123456789ABCDEF"[u >> 4];
                out += "0123456789ABCDEF"[u & 15];
            } else if (c == ' ' && fold >= 0 && i > 0 && i + 1 < s.size() &&
                       s[i - 1] != ' ' && s[i + 1] != ' ' && rng_.chance(30)) {
                out += '\n';
                out += dftracer::utils::dstd::String(
                    static_cast<std::size_t>(fold), ' ');
                counted_quoted_ = true;
            } else {
                out += c;
            }
        }
        if (counted_quoted_) ++stats_.multi_line_quoted;
        counted_quoted_ = false;
        return out + "\"";
    }

    // A scalar on one line or across lines (`fold` the continuation indent,
    // or -1 for one line) that reads back as `s`.
    dftracer::utils::dstd::String scalar_text(
        const dftracer::utils::dstd::String& s, int fold, bool in_flow) {
        if (is_plain_safe(s, in_flow) && rng_.chance(60)) {
            dftracer::utils::dstd::String out = s;
            if (fold >= 0 && !in_flow && rng_.chance(20)) {
                for (char& c : out)
                    if (c == ' ' && rng_.chance(40)) c = '\n';
                dftracer::utils::dstd::String folded;
                for (char c : out) {
                    folded += c;
                    if (c == '\n')
                        folded += dftracer::utils::dstd::String(
                            static_cast<std::size_t>(fold), ' ');
                }
                // A line that ends the scalar before the fold must not be
                // a bare number of spaces.
                if (folded.find('\n') != dftracer::utils::dstd::String::npos)
                    ++stats_.multi_line_plain;
                return folded;
            }
            return out;
        }
        bool plain_chars = true;
        for (char c : s) {
            const auto u = static_cast<unsigned char>(c);
            plain_chars &= u >= 0x20 && u != 0x7F && c != '\t';
        }
        if (plain_chars && rng_.chance(50)) return single_quoted(s);
        return double_quoted(s, fold);
    }

    void indent(int n) {
        out_ += dftracer::utils::dstd::String(static_cast<std::size_t>(n), ' ');
    }

    // Moves the value of an entry at `parent` to its own line, after blank and
    // comment lines, and writes its indent, which is returned.
    int gap_to_next_line(int parent) {
        out_ += '\n';
        const std::size_t lines = 1 + rng_.below(2);
        for (std::size_t l = 0; l < lines; ++l) {
            if (rng_.chance(50)) {
                out_ += '\n';
            } else {
                indent(rng_.chance(50) ? parent + 1 : 0);
                out_ += "# gap\n";
            }
            ++stats_.gap_lines;
        }
        const int at = parent + 1 + static_cast<int>(rng_.below(3));
        indent(at);
        return at;
    }

    void maybe_comment() {
        if (rng_.chance(8)) out_ += " # a comment";
    }

    void maybe_comment_line(int n) {
        if (rng_.chance(6)) {
            indent(rng_.chance(50) ? n : 0);
            out_ += "# between\n";
            no_blank_ = false;
        }
        if (!no_blank_ && rng_.chance(3)) out_ += "\n";
        no_blank_ = false;
    }

    void root_scalar(const Tree& t) {
        const dftracer::utils::dstd::String& s = t.text;
        if (block_scalar_ok(s) && rng_.chance(30)) {
            block_scalar(s, -1, true);
            return;
        }
        out_ += scalar_text(s, 1, false);
        out_ += '\n';
    }

    // A literal block scalar needs lines of text that start with no space.
    static bool block_scalar_ok(const dftracer::utils::dstd::String& s) {
        if (s.empty() || s.front() == ' ' || s.front() == '\n') return false;
        bool line_start = true;
        for (char c : s) {
            const auto u = static_cast<unsigned char>(c);
            if (u < 0x20 && c != '\n') return false;
            if (u == 0x7F) return false;
            if (line_start && c == ' ') return false;
            line_start = c == '\n';
        }
        return true;
    }

    // Writes a literal block scalar for `s`, after a header on the current
    // line; `parent` is the indent of its parent, -1 at the root.
    void block_scalar(const dftracer::utils::dstd::String& s, int parent,
                      bool at_root) {
        std::size_t trailing = 0;
        while (trailing < s.size() && s[s.size() - 1 - trailing] == '\n')
            ++trailing;
        const dftracer::utils::dstd::String body =
            s.substr(0, s.size() - trailing);
        ++stats_.block_scalars;
        out_ += at_root ? "" : " ";
        out_ += "|";
        out_ += trailing == 0 ? "-" : (trailing == 1 ? "" : "+");
        maybe_comment();
        out_ += '\n';
        int pad = parent + 1 + static_cast<int>(rng_.below(3));
        if (pad < 1) pad = 1;
        std::size_t i = 0;
        for (;;) {
            const std::size_t nl = body.find('\n', i);
            const std::size_t stop =
                nl == dftracer::utils::dstd::String::npos ? body.size() : nl;
            if (stop > i) indent(pad);
            out_ += body.substr(i, stop - i);
            out_ += '\n';
            if (nl == dftracer::utils::dstd::String::npos) break;
            i = stop + 1;
        }
        for (std::size_t k = 1; k < trailing; ++k) out_ += '\n';
        no_blank_ = trailing > 1;
        block_end_ = out_.size();
    }

    void key_text(const dftracer::utils::dstd::String& key) {
        if (is_plain_safe(key, false) && rng_.chance(70)) {
            out_ += key;
        } else {
            bool plain_chars = true;
            for (char c : key) {
                const auto u = static_cast<unsigned char>(c);
                plain_chars &= u >= 0x20 && u != 0x7F && c != '\t';
            }
            out_ += plain_chars && rng_.chance(50) ? single_quoted(key)
                                                   : double_quoted(key, -1);
        }
    }

    // The value of a map entry or sequence item whose indicator is already
    // written, and then the rest of the line. `parent` is the indent of the
    // entry.
    void value(const Tree& t, int parent, bool is_item) {
        switch (t.kind) {
            case Tree::Kind::Null:
                if (rng_.chance(40)) {
                    maybe_comment();
                } else {
                    out_ += rng_.chance(50) ? " ~" : " null";
                }
                out_ += '\n';
                return;
            case Tree::Kind::String:
                if (rng_.chance(12)) {
                    const int at = gap_to_next_line(parent);
                    if (block_scalar_ok(t.text) && rng_.chance(25)) {
                        ++stats_.gap_block_scalars;
                        block_scalar(t.text, parent, true);
                    } else {
                        ++stats_.gap_scalars;
                        out_ += scalar_text(t.text, at, false);
                        maybe_comment_if_single();
                        out_ += '\n';
                    }
                } else if (block_scalar_ok(t.text) && rng_.chance(25)) {
                    block_scalar(t.text, parent, false);
                } else {
                    out_ += ' ';
                    out_ += scalar_text(t.text, parent + 2, false);
                    maybe_comment_if_single();
                    out_ += '\n';
                }
                return;
            default:
                break;
        }
        if (rng_.chance(30)) {
            int at = parent + 1;
            if (rng_.chance(12)) {
                ++stats_.gap_flows;
                at = gap_to_next_line(parent);
            } else {
                out_ += ' ';
            }
            flow_node(t, at, at);
            maybe_comment();
            out_ += '\n';
            return;
        }
        if (is_item && rng_.chance(50)) {
            // The collection starts on the line of the dash.
            out_ += ' ';
            block_collection(t, parent + 2, true);
            return;
        }
        out_ += '\n';
        block_collection(t, parent + 2 + static_cast<int>(rng_.below(2)),
                         false);
    }

    void maybe_comment_if_single() {
        // A comment after a scalar of several lines would end it, so it goes
        // only after the last line, which is where the text ends.
        maybe_comment();
    }

    // A map or a sequence in block style at column `n`. `inline_first` says
    // that the first entry starts where the output is, in a sequence item.
    void block_collection(const Tree& t, int n, bool inline_first) {
        if (t.items.empty()) {
            if (inline_first) {
                out_ += t.kind == Tree::Kind::Map ? "{}\n" : "[]\n";
            } else {
                indent(n);
                out_ += t.kind == Tree::Kind::Map ? "{}\n" : "[]\n";
            }
            return;
        }
        for (std::size_t i = 0; i < t.items.size(); ++i) {
            if (!(inline_first && i == 0)) {
                maybe_comment_line(n);
                indent(n);
            }
            if (t.kind == Tree::Kind::Map) {
                key_text(t.keys[i]);
                out_ += ':';
                const Tree& v = t.items[i];
                if (v.kind == Tree::Kind::Seq && !v.items.empty() &&
                    rng_.chance(30)) {
                    // A sequence at the column of its key.
                    out_ += '\n';
                    block_collection(v, n, false);
                } else {
                    value(v, n, false);
                }
            } else {
                out_ += '-';
                value(t.items[i], n, true);
            }
        }
    }

    void flow_break(int n) {
        if (rng_.chance(15)) {
            if (rng_.chance(20)) out_ += " # in flow";
            out_ += '\n';
            ++stats_.flow_multi_line;
            indent(n + static_cast<int>(rng_.below(3)));
        } else if (rng_.chance(40)) {
            out_ += ' ';
        }
    }

    // A flow collection or scalar. `n` is the least indent of a line after the
    // first.
    void flow_node(const Tree& t, int n, int fold) {
        switch (t.kind) {
            case Tree::Kind::Null:
            case Tree::Kind::Bool:
            case Tree::Kind::Number:
                out_ += rng_.chance(50) ? "null" : "~";
                return;
            case Tree::Kind::String:
                out_ += scalar_text(t.text, fold, true);
                return;
            case Tree::Kind::Seq:
                ++stats_.flow_collections;
                out_ += '[';
                flow_break(n);
                for (std::size_t i = 0; i < t.items.size(); ++i) {
                    const Tree& item = t.items[i];
                    if (item.kind == Tree::Kind::Map &&
                        item.items.size() == 1 && rng_.chance(40) &&
                        item.items[0].kind != Tree::Kind::Seq &&
                        item.items[0].kind != Tree::Kind::Map) {
                        key_text_flow(item.keys[0]);
                        out_ += ": ";
                        flow_node(item.items[0], n, n);
                    } else {
                        flow_node(item, n, n);
                    }
                    const bool last = i + 1 == t.items.size();
                    if (!last || rng_.chance(15)) out_ += ',';
                    if (!last || rng_.chance(20)) flow_break(n);
                }
                out_ += ']';
                return;
            case Tree::Kind::Map:
                ++stats_.flow_collections;
                out_ += '{';
                flow_break(n);
                for (std::size_t i = 0; i < t.items.size(); ++i) {
                    const Tree& value = t.items[i];
                    key_text_flow(t.keys[i]);
                    if (value.kind == Tree::Kind::Null && rng_.chance(40)) {
                        // A key with no value.
                    } else if (rng_.chance(15) && !is_plain_key(t.keys[i])) {
                        out_ += ':';
                        flow_node(value, n, n);
                    } else {
                        out_ += ": ";
                        flow_node(value, n, n);
                    }
                    const bool last = i + 1 == t.items.size();
                    if (!last || rng_.chance(15)) out_ += ',';
                    if (!last || rng_.chance(20)) flow_break(n);
                }
                out_ += '}';
                return;
        }
    }

    bool is_plain_key(const dftracer::utils::dstd::String& key) {
        return is_plain_safe(key, true);
    }

    void key_text_flow(const dftracer::utils::dstd::String& key) {
        if (is_plain_safe(key, true) && rng_.chance(60)) {
            out_ += key;
            return;
        }
        bool plain_chars = true;
        for (char c : key) {
            const auto u = static_cast<unsigned char>(c);
            plain_chars &= u >= 0x20 && u != 0x7F && c != '\t';
        }
        out_ += plain_chars && rng_.chance(50) ? single_quoted(key)
                                               : double_quoted(key, -1);
    }

    Rng rng_;
    dftracer::utils::dstd::String out_;
    bool no_blank_ = false;
    bool counted_quoted_ = false;
    std::size_t block_end_ = ~std::size_t{0};
    Stats stats_;
};

}  // namespace dftu_yaml_test

#endif  // DFTRACER_UTILS_TESTS_YAML_GEN_H
