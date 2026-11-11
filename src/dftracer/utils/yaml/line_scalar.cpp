// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/yaml/line_scalar.h>

#include <cstring>

namespace dftracer::utils::yaml::detail {

namespace {

char* grow(LineIndex& ix, std::size_t n) {
    const std::size_t old = ix.arena.size();
    ix.arena.resize(old + n);
    return ix.arena.data() + old;
}

char* append_utf8(char* w, std::uint32_t cp) {
    if (cp < 0x80) {
        *w++ = static_cast<char>(cp);
    } else if (cp < 0x800) {
        *w++ = static_cast<char>(0xC0 | (cp >> 6));
        *w++ = static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *w++ = static_cast<char>(0xE0 | (cp >> 12));
        *w++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *w++ = static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        *w++ = static_cast<char>(0xF0 | (cp >> 18));
        *w++ = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        *w++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *w++ = static_cast<char>(0x80 | (cp & 0x3F));
    }
    return w;
}

int hex_value(char c) {
    if (dstd::is_ascii_digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decodes `[s, e)` of a double quoted scalar into `w`. `barrier` ends up after
// the last escape, which a fold must not trim. `escaped_break` is set when the
// segment ends in a backslash: the line break after it is escaped.
Decoded decode_double(const char* t, std::uint32_t s, std::uint32_t e, char*& w,
                      char*& barrier, bool& escaped_break,
                      EscapeProblem& problem) {
    for (std::uint32_t i = s; i < e; ++i) {
        if (t[i] != '\\') {
            *w++ = t[i];
            continue;
        }
        const std::uint32_t at = i;
        ++i;
        if (i >= e) {
            escaped_break = true;
            return Decoded::Ok;
        }
        std::uint32_t cp = 0;
        switch (t[i]) {
            case '\\':
            case '"':
            case '/':
                *w++ = t[i];
                barrier = w;
                continue;
            case 'n':
                *w++ = '\n';
                barrier = w;
                continue;
            case 't':
                *w++ = '\t';
                barrier = w;
                continue;
            case 'r':
                *w++ = '\r';
                barrier = w;
                continue;
            case 'b':
                *w++ = '\b';
                barrier = w;
                continue;
            case 'f':
                *w++ = '\f';
                barrier = w;
                continue;
            case 'a':
                *w++ = '\a';
                barrier = w;
                continue;
            case 'e':
                *w++ = '\x1B';
                barrier = w;
                continue;
            case 'v':
                *w++ = '\v';
                barrier = w;
                continue;
            case '0':
            case ' ':
            case '_':
            case 'N':
            case 'L':
            case 'P':
                return Decoded::Decline;
            case 'x':
            case 'u':
            case 'U': {
                const std::uint32_t digits =
                    t[i] == 'x' ? 2u : (t[i] == 'u' ? 4u : 8u);
                if (e - i - 1 < digits) {
                    problem.kind = EscapeProblem::Digits;
                    problem.at = at;
                    problem.ch = t[i];
                    problem.count = digits;
                    return Decoded::Invalid;
                }
                for (std::uint32_t d = 0; d < digits; ++d) {
                    const int h = hex_value(t[i + 1 + d]);
                    if (h < 0) {
                        problem.kind = EscapeProblem::Hex;
                        problem.at = at;
                        problem.ch = t[i];
                        problem.count = digits;
                        return Decoded::Invalid;
                    }
                    cp = (cp << 4) | static_cast<std::uint32_t>(h);
                }
                i += digits;
                if (cp > 0x10FFFF) {
                    problem.kind = EscapeProblem::Range;
                    problem.at = at;
                    problem.ch = t[i - digits];
                    problem.count = digits;
                    return Decoded::Invalid;
                }
                // A surrogate, NUL, and the code points a YAML reader takes
                // for a line break or refuses, which the tape reader folds in
                // its own way.
                if (cp == 0 || (cp >= 0xD800 && cp < 0xE000) || cp == 0x7F ||
                    (cp >= 0x80 && cp <= 0xA0) || cp == 0x2028 ||
                    cp == 0x2029 || cp == 0xFEFF || cp == 0xFFFE ||
                    cp == 0xFFFF)
                    return Decoded::Decline;
                break;
            }
            default:
                problem.kind = EscapeProblem::Unknown;
                problem.at = at;
                problem.ch = t[i];
                return Decoded::Invalid;
        }
        w = append_utf8(w, cp);
        barrier = w;
    }
    return Decoded::Ok;
}

void decode_single(const char* t, std::uint32_t s, std::uint32_t e, char*& w) {
    for (std::uint32_t i = s; i < e; ++i) {
        *w++ = t[i];
        if (t[i] == '\'') ++i;
    }
}

void line_bounds(const LineIndex& ix, std::uint32_t line, std::uint32_t& s,
                 std::uint32_t& e) {
    s = ix.entries[line].start;
    e = ix.line_end(line);
}

}  // namespace

Decoded decode_quoted(LineIndex& ix, std::uint32_t s, std::uint32_t e,
                      bool double_quoted, EscapeProblem& problem,
                      ScalarText& out) {
    const std::uint32_t old = static_cast<std::uint32_t>(ix.arena.size());
    char* base = grow(ix, e - s);
    char* w = base;
    if (double_quoted) {
        char* barrier = w;
        bool escaped_break = false;
        const Decoded decoded =
            decode_double(ix.text, s, e, w, barrier, escaped_break, problem);
        if (decoded != Decoded::Ok) {
            ix.arena.resize(old);
            return decoded;
        }
    } else {
        decode_single(ix.text, s, e, w);
    }
    out.off = old;
    out.len = static_cast<std::uint32_t>(w - base);
    out.in_text = false;
    ix.arena.resize(old + out.len);
    return Decoded::Ok;
}

// Folds the lines of a quoted scalar: trailing white space before a break and
// leading white space after it go, one break is a space, each empty line is a
// line feed, and an escaped break joins the lines (YAML 1.2.2, 6.5 line
// folding and 7.3.1 and 7.3.2, https://yaml.org/spec/1.2.2/).
Decoded decode_quoted_lines(LineIndex& ix, std::uint32_t first,
                            std::uint32_t last, std::uint32_t quote_at,
                            std::uint32_t close_at, EscapeProblem& problem,
                            ScalarText& out) {
    const char* t = ix.text;
    const bool is_double = t[quote_at] == '"';
    const std::uint32_t old = static_cast<std::uint32_t>(ix.arena.size());
    char* base = grow(ix, close_at - quote_at);
    char* w = base;
    char* barrier = w;
    std::uint32_t empties = 0;
    bool after_escaped = false;
    for (std::uint32_t line = first; line <= last; ++line) {
        std::uint32_t s = 0;
        std::uint32_t e = 0;
        line_bounds(ix, line, s, e);
        std::uint32_t from = quote_at + 1;
        if (line != first) {
            from = s;
            while (from < e && t[from] == ' ') ++from;
        }
        const bool is_last = line == last;
        if (line != first && !is_last && from == e) {
            ++empties;
            continue;
        }
        if (line != first) {
            if (empties != 0) {
                std::memset(w, '\n', empties);
                w += empties;
            } else if (!after_escaped) {
                *w++ = ' ';
            }
            empties = 0;
            after_escaped = false;
        }
        barrier = w;
        bool escaped_break = false;
        const std::uint32_t to = is_last ? close_at : e;
        if (is_double) {
            const Decoded decoded =
                decode_double(t, from, to, w, barrier, escaped_break, problem);
            if (decoded != Decoded::Ok) {
                problem.line = line;
                ix.arena.resize(old);
                return decoded;
            }
        } else {
            decode_single(t, from, to, w);
        }
        if (!is_last) {
            if (escaped_break) {
                after_escaped = true;
            } else {
                while (w > barrier && w[-1] == ' ') --w;
            }
        }
    }
    out.off = old;
    out.len = static_cast<std::uint32_t>(w - base);
    out.in_text = false;
    ix.arena.resize(old + out.len);
    return Decoded::Ok;
}

// Folds the lines of a plain scalar as 7.3.3 and 6.5 of YAML 1.2.2 say
// (https://yaml.org/spec/1.2.2/).
void decode_plain_lines(LineIndex& ix, std::uint32_t first, std::uint32_t last,
                        std::uint32_t first_s, std::uint32_t first_e,
                        std::uint32_t last_e, ScalarText& out) {
    const char* t = ix.text;
    const std::uint32_t old = static_cast<std::uint32_t>(ix.arena.size());
    char* base = grow(ix, last_e - first_s);
    char* w = base;
    std::memcpy(w, t + first_s, first_e - first_s);
    w += first_e - first_s;
    std::uint32_t empties = 0;
    for (std::uint32_t line = first + 1; line <= last; ++line) {
        std::uint32_t s = 0;
        std::uint32_t e = 0;
        line_bounds(ix, line, s, e);
        while (s < e && t[s] == ' ') ++s;
        if (s == e) {
            ++empties;
            continue;
        }
        std::uint32_t te = line == last ? last_e : e;
        if (line != last) {
            while (te > s && t[te - 1] == ' ') --te;
        }
        if (empties != 0) {
            std::memset(w, '\n', empties);
            w += empties;
            empties = 0;
        } else {
            *w++ = ' ';
        }
        std::memcpy(w, t + s, te - s);
        w += te - s;
    }
    out.off = old;
    out.len = static_cast<std::uint32_t>(w - base);
    out.in_text = false;
    ix.arena.resize(old + out.len);
}

// A block scalar: the lines are literal (8.1.2) or folded, where a break
// between two lines of text is a space unless a line starts with white space
// (8.1.3), and the chomping indicator keeps, clips or strips the final breaks
// (8.1.1.2). YAML 1.2.2, https://yaml.org/spec/1.2.2/. The text is built after
// the lines are scanned, as rapidyaml filters a scanned block (MIT,
// https://github.com/biojppm/rapidyaml).
Decoded decode_block(LineIndex& ix, const BlockSpec& spec, std::uint32_t first,
                     std::uint32_t end, ScalarText& out) {
    const char* t = ix.text;
    const std::uint32_t ci = spec.indent;
    const bool is_unterminated =
        end == ix.line_count && ix.entries[end].start > ix.size;
    const std::uint32_t span_end =
        end == first ? ix.entries[first].start
                     : (is_unterminated ? ix.size : ix.entries[end].start);
    const std::uint32_t span = span_end - ix.entries[first].start;
    const std::uint32_t old = static_cast<std::uint32_t>(ix.arena.size());
    char* base = grow(ix, static_cast<std::size_t>(span) + 2);
    char* w = base;
    std::uint32_t pending = 0;
    bool have = false;
    bool prev_spaced = false;
    std::uint32_t first_text = 0;
    for (std::uint32_t line = first; line < end; ++line) {
        std::uint32_t s = 0;
        std::uint32_t e = 0;
        line_bounds(ix, line, s, e);
        std::uint32_t i = s;
        while (i < e && t[i] == ' ') ++i;
        const std::uint32_t spaces = i - s;
        const bool is_blank = i == e;
        if (is_blank && !(spec.is_indent_known && spaces > ci)) {
            ++pending;
            continue;
        }
        const std::uint32_t text_at = s + ci;
        const bool is_spaced =
            text_at < e && (t[text_at] == ' ' || t[text_at] == '\t');
        std::uint32_t breaks = 0;
        if (!have) {
            breaks = pending;
            first_text = text_at;
        } else if (spec.is_literal || prev_spaced || is_spaced) {
            breaks = pending + 1;
        } else if (pending == 0) {
            *w++ = ' ';
        } else {
            breaks = pending;
        }
        if (breaks != 0) {
            std::memset(w, '\n', breaks);
            w += breaks;
        }
        std::memcpy(w, t + text_at, e - text_at);
        w += e - text_at;
        pending = 0;
        have = true;
        prev_spaced = is_spaced;
    }
    if (is_unterminated && pending != 0 && spec.chomp > 0) {
        ix.arena.resize(old);
        return Decoded::Decline;
    }
    if (have) {
        if (spec.chomp >= 0 && !is_unterminated) *w++ = '\n';
        if (spec.chomp > 0 && pending != 0) {
            std::memset(w, '\n', pending);
            w += pending;
        }
    } else if (spec.chomp > 0 && pending != 0) {
        std::memset(w, '\n', pending);
        w += pending;
    }
    const auto len = static_cast<std::uint32_t>(w - base);
    out.len = len;
    // The text of a block that is a slice of the text needs no copy.
    const std::uint32_t candidate =
        ci == 0 ? ix.entries[first].start : first_text;
    if (have && len != 0 && candidate + len <= ix.size &&
        std::memcmp(t + candidate, base, len) == 0) {
        ix.arena.resize(old);
        out.off = candidate;
        out.in_text = true;
        return Decoded::Ok;
    }
    out.off = old;
    out.in_text = false;
    ix.arena.resize(old + len);
    return Decoded::Ok;
}

}  // namespace dftracer::utils::yaml::detail
