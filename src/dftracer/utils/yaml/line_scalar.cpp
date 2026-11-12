// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/fmt.h>
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
                *w++ = '\0';
                barrier = w;
                continue;
            case ' ':
            case '\t':
                *w++ = t[i];
                barrier = w;
                continue;
            case '_':
                cp = 0xA0;
                break;
            case 'N':
                cp = 0x85;
                break;
            case 'L':
                cp = 0x2028;
                break;
            case 'P':
                cp = 0x2029;
                break;
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
                if (cp >= 0xD800 && cp < 0xDC00 && digits == 4 && i + 6 < e &&
                    t[i + 1] == '\\' && t[i + 2] == 'u') {
                    std::uint32_t low = 0;
                    bool is_low = true;
                    for (std::uint32_t d = 0; d < 4 && is_low; ++d) {
                        const int h = hex_value(t[i + 3 + d]);
                        is_low = h >= 0;
                        low = (low << 4) | static_cast<std::uint32_t>(h);
                    }
                    if (is_low && low >= 0xDC00 && low < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        i += 6;
                    }
                }
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) {
                    problem.kind = EscapeProblem::Range;
                    problem.at = at;
                    problem.ch = t[at + 1];
                    problem.count = digits;
                    return Decoded::Invalid;
                }
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

bool is_white(char c) { return c == ' ' || c == '\t'; }

}  // namespace

dstd::String escape_reason(const EscapeProblem& problem) {
    switch (problem.kind) {
        case EscapeProblem::Unknown:
            return dstd::fmt<"unknown escape character '{}'">(
                dstd::StringView(&problem.ch, 1));
        case EscapeProblem::Digits:
            return dstd::fmt<"escape '\\{}' needs {} hex digits">(
                dstd::StringView(&problem.ch, 1), problem.count);
        case EscapeProblem::Hex:
            return dstd::fmt<
                "escape '\\{}' has a character that is not a hex digit">(
                dstd::StringView(&problem.ch, 1));
        default:
            return "escape is not a code point";
    }
}

// The offset of the first line feed in `[pos, e)`, or `e`. A line is short, so
// a few bytes are looked at before a call is worth it.
inline std::uint32_t find_newline(const char* t, std::uint32_t pos,
                                  std::uint32_t e) {
    const std::uint32_t near_end = e - pos < 24 ? e : pos + 24;
    for (std::uint32_t i = pos; i < near_end; ++i)
        if (t[i] == '\n') return i;
    if (near_end == e) return e;
    const void* found = std::memchr(t + near_end, '\n', e - near_end);
    return found == nullptr ? e
                            : static_cast<std::uint32_t>(
                                  static_cast<const char*>(found) - t);
}

// Folds the lines of a quoted scalar: trailing white space before a break and
// leading white space after it go, one break is a space, each empty line is a
// line feed, and an escaped break joins the lines (YAML 1.2.2, 6.5 line
// folding and 7.3.1 and 7.3.2, https://yaml.org/spec/1.2.2/).
Decoded decode_quoted(LineIndex& ix, std::uint32_t s, std::uint32_t e,
                      bool double_quoted, EscapeProblem& problem,
                      ScalarText& out) {
    const char* t = ix.text;
    const std::uint32_t old = static_cast<std::uint32_t>(ix.arena.size());
    const std::size_t room = static_cast<std::size_t>(e - s) * 3 / 2 + 4;
    char* base = grow(ix, room);
    char* w = base;
    std::uint32_t pos = s;
    std::uint32_t empties = 0;
    bool after_escaped = false;
    bool is_first = true;
    for (;;) {
        const std::uint32_t line_to = find_newline(t, pos, e);
        const bool is_last = line_to == e;
        std::uint32_t from = pos;
        std::uint32_t to = line_to;
        if (!is_last && to > from && t[to - 1] == '\r') --to;
        if (!is_first) {
            while (from < to && is_white(t[from])) ++from;
            if (!is_last && from == to) {
                ++empties;
                pos = line_to + 1;
                continue;
            }
            if (empties != 0) {
                std::memset(w, '\n', empties);
                w += empties;
            } else if (!after_escaped) {
                *w++ = ' ';
            }
            empties = 0;
            after_escaped = false;
        }
        char* barrier = w;
        bool escaped_break = false;
        if (double_quoted) {
            const Decoded decoded =
                decode_double(t, from, to, w, barrier, escaped_break, problem);
            if (decoded != Decoded::Ok) {
                ix.arena.resize(old);
                return decoded;
            }
        } else {
            decode_single(t, from, to, w);
        }
        if (is_last) break;
        if (escaped_break) {
            after_escaped = true;
        } else {
            while (w > barrier && is_white(w[-1])) --w;
        }
        pos = line_to + 1;
        is_first = false;
    }
    out.off = old;
    out.len = static_cast<std::uint32_t>(w - base);
    out.in_text = false;
    ix.arena.resize(old + out.len);
    return Decoded::Ok;
}

// The same as @ref decode_plain for a scalar of the lines `first` to `last` of
// the line index: the first text of the first line is `[first_s, first_e)` and
// the last line ends its text at `last_e`. The lines are read from the line
// table, so no line feed is looked for.
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
        while (s < e && is_white(t[s])) ++s;
        if (s == e) {
            ++empties;
            continue;
        }
        std::uint32_t te = line == last ? last_e : e;
        if (line != last) {
            while (te > s && is_white(t[te - 1])) --te;
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

// Folds the lines of a plain scalar as 7.3.3 and 6.5 of YAML 1.2.2 say
// (https://yaml.org/spec/1.2.2/).
void decode_plain(LineIndex& ix, std::uint32_t s, std::uint32_t e,
                  ScalarText& out) {
    const char* t = ix.text;
    const std::uint32_t old = static_cast<std::uint32_t>(ix.arena.size());
    char* base = grow(ix, e - s);
    char* w = base;
    std::uint32_t pos = s;
    std::uint32_t empties = 0;
    bool is_first = true;
    for (;;) {
        const std::uint32_t line_to = find_newline(t, pos, e);
        const bool is_last = line_to == e;
        std::uint32_t from = pos;
        std::uint32_t to = line_to;
        if (!is_first) {
            while (from < to && is_white(t[from])) ++from;
        }
        if (!is_last) {
            while (to > from && (is_white(t[to - 1]) || t[to - 1] == '\r'))
                --to;
        }
        if (!is_first && from == to) {
            ++empties;
        } else {
            if (!is_first) {
                if (empties != 0) {
                    std::memset(w, '\n', empties);
                    w += empties;
                    empties = 0;
                } else {
                    *w++ = ' ';
                }
            }
            std::memcpy(w, t + from, to - from);
            w += to - from;
        }
        if (is_last) break;
        pos = line_to + 1;
        is_first = false;
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
    // The last line has text that is not only spaces: a text that ends there
    // has no line feed to keep (YAML 1.2.2, b-chomped-last).
    bool ends_in_text = false;
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
            ends_in_text = false;
            continue;
        }
        ends_in_text = !is_blank;
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
    if (have) {
        if (spec.chomp >= 0 && !(is_unterminated && ends_in_text)) *w++ = '\n';
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
