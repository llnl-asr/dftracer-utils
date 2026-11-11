// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/bits.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/yaml/line_index.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace dftracer::utils::yaml {

namespace {

constexpr std::uint32_t NONE = 0xFFFFFFFFu;
constexpr std::size_t MAX_DEPTH = 128;
/// A map with fewer keys than this checks a new key against the earlier ones
/// after a bloom hit; a map with more uses a hash table.
constexpr std::size_t LINEAR_KEY_LIMIT = 64;
constexpr std::size_t BATCH_BLOCKS = 64;
constexpr std::uint64_t TEXT_SIZE_LIMIT = 0xFFFFFFF0ull;
constexpr std::uint32_t INDENT_LIMIT = 0xFFFE;
/// A text with lines shorter than this on average is left to the tape reader,
/// which sizes its tape from them better: the index is 12 bytes a line.
constexpr std::uint32_t MIN_AVERAGE_LINE = 4;

bool is_bad_lead(char c) {
    switch (c) {
        case '&':
        case '*':
        case '!':
        case '?':
        case '|':
        case '>':
        case '%':
        case '@':
        case '`':
        case ']':
        case '}':
        case ',':
        case ':':
            return true;
        default:
            return false;
    }
}

bool is_flow_char(char c) {
    return c == '[' || c == ']' || c == '{' || c == '}' || c == ',';
}

struct Quoted {
    std::uint32_t inner_end = 0;
    std::uint32_t after = 0;
    bool esc = false;
};

// The quoted scalar that opens at `p`, closed on the same line.
bool scan_quoted(const char* t, std::uint32_t p, std::uint32_t end, Quoted& q) {
    const char quote = t[p];
    std::uint32_t i = p + 1;
    if (quote == '\'') {
        while (i < end) {
            if (t[i] == '\'') {
                if (i + 1 < end && t[i + 1] == '\'') {
                    q.esc = true;
                    i += 2;
                    continue;
                }
                q.inner_end = i;
                q.after = i + 1;
                return true;
            }
            ++i;
        }
        return false;
    }
    while (i < end) {
        const char c = t[i];
        if (c == '\\') {
            q.esc = true;
            if (i + 1 >= end) return false;
            i += 2;
            continue;
        }
        if (c == '"') {
            q.inner_end = i;
            q.after = i + 1;
            return true;
        }
        ++i;
    }
    return false;
}

// What follows a scalar: nothing, or a comment set off by a space.
bool ends_in_comment_or_nothing(const char* t, std::uint32_t after,
                                std::uint32_t end, bool& has_comment,
                                std::uint32_t& stray) {
    std::uint32_t a = after;
    while (a < end && t[a] == ' ') ++a;
    has_comment = a < end;
    stray = a;
    if (a == end) return true;
    return t[a] == '#' && a > after;
}

// Whether a quote of the kind `quote` closes anywhere in `[from, size)`.
bool closes_later(const char* t, std::uint32_t from, std::uint32_t size,
                  char quote) {
    for (std::uint32_t i = from; i < size; ++i) {
        if (quote == '"' && t[i] == '\\') {
            ++i;
        } else if (t[i] == quote) {
            return true;
        }
    }
    return false;
}

// A quoted scalar that does not close on its line is a multi-line scalar,
// which the line index declines, unless nothing closes it in the whole text.
bool unclosed(const char* t, std::uint32_t quote_at, std::uint32_t end,
              std::uint32_t text_size, LineParts& pt) {
    if (!closes_later(t, end, text_size, t[quote_at])) {
        pt.error = LineError::Unclosed;
        pt.error_at = quote_at;
    }
    return false;
}

// `[]` or `{}` that starts at `p`, then nothing or a comment.
bool is_empty_flow(const char* t, std::uint32_t p, std::uint32_t end,
                   bool& has_comment) {
    const char close = t[p] == '[' ? ']' : '}';
    if (p + 1 >= end || t[p + 1] != close) return false;
    std::uint32_t stray = 0;
    return ends_in_comment_or_nothing(t, p + 2, end, has_comment, stray);
}

// `~`, `null`, `Null` and `NULL`, which the tape reader reads as a null key,
// and `<<`, its merge key.
bool is_null_key(dstd::StringView key) {
    const char* k = key.data();
    switch (key.size()) {
        case 1:
            return k[0] == '~';
        case 2:
            return k[0] == '<' && k[1] == '<';
        case 4: {
            std::uint32_t word = 0;
            std::memcpy(&word, k, 4);
            return word == 0x6C6C756Eu || word == 0x6C6C754Eu ||
                   word == 0x4C4C554Eu;
        }
        default:
            return false;
    }
}

}  // namespace

bool classify_line(const char* t, std::uint32_t start, std::uint32_t end,
                   std::uint32_t text_size, LineParts& pt) {
    pt = LineParts{};
    std::uint32_t i = start;
    while (i < end && t[i] == ' ') ++i;
    pt.indent = i - start;
    if (pt.indent > INDENT_LIMIT) return false;
    if (i == end) {
        pt.kind = LineKind::Blank;
        return true;
    }
    if (t[i] == '#') {
        pt.kind = LineKind::Comment;
        return true;
    }
    if (pt.indent == 0 && (t[i] == '-' || t[i] == '.') && end - i >= 3 &&
        t[i + 1] == t[i] && t[i + 2] == t[i]) {
        const std::uint32_t after = i + 3;
        if (after < end && t[after] != ' ') return false;
        std::uint32_t k = after;
        while (k < end && t[k] == ' ') ++k;
        if (k < end && t[k] != '#') return false;
        pt.kind = t[i] == '-' ? LineKind::DocStart : LineKind::DocEnd;
        return true;
    }
    bool seq = false;
    std::uint32_t p = i;
    if (t[i] == '-' && (i + 1 == end || t[i + 1] == ' ')) {
        seq = true;
        pt.dash = i;
        p = i + 1;
        while (p < end && t[p] == ' ') ++p;
        pt.kind = LineKind::SeqItem;
        pt.content = p;
        if (p == end) {
            pt.flags = line_flag::VAL_EMPTY;
            return true;
        }
        if (t[p] == '#') {
            pt.flags = line_flag::VAL_EMPTY | line_flag::COMMENT;
            return true;
        }
        if (t[p] == '-' && (p + 1 == end || t[p + 1] == ' ')) return false;
    }
    pt.content = p;
    const char c = t[p];
    std::uint32_t val_from = 0;
    if (c == '"' || c == '\'') {
        Quoted q;
        if (!scan_quoted(t, p, end, q))
            return unclosed(t, p, end, text_size, pt);
        std::uint32_t a = q.after;
        while (a < end && t[a] == ' ') ++a;
        if (a < end && t[a] == ':' && (a + 1 == end || t[a + 1] == ' ')) {
            pt.key_s = p + 1;
            pt.key_e = q.inner_end;
            pt.flags |= line_flag::KEY_QUOTED;
            if (q.esc) pt.flags |= line_flag::KEY_ESC;
            val_from = a + 1;
        } else {
            if (!seq) return false;
            bool has_comment = false;
            std::uint32_t stray = 0;
            if (!ends_in_comment_or_nothing(t, q.after, end, has_comment,
                                            stray))
                return false;
            pt.kind = LineKind::SeqItem;
            pt.val_s = p + 1;
            pt.val_e = q.inner_end;
            pt.flags |= line_flag::VAL_QUOTED;
            if (q.esc) pt.flags |= line_flag::VAL_ESC;
            if (has_comment) pt.flags |= line_flag::COMMENT;
            return true;
        }
    } else {
        if (is_bad_lead(c)) return false;
        if (c == '[' || c == '{') {
            bool has_comment = false;
            if (!seq || !is_empty_flow(t, p, end, has_comment)) return false;
            pt.kind = LineKind::SeqItem;
            pt.val_s = p;
            pt.val_e = p + 2;
            pt.flags |= line_flag::FLOW_EMPTY;
            if (has_comment) pt.flags |= line_flag::COMMENT;
            return true;
        }
        std::uint32_t sep = NONE;
        std::uint32_t comment = NONE;
        for (std::uint32_t k = p; k < end; ++k) {
            const char ch = t[k];
            if (ch == ':' && (k + 1 == end || t[k + 1] == ' ')) {
                sep = k;
                break;
            }
            if (ch == ' ' && k + 1 < end && t[k + 1] == '#') {
                comment = k;
                break;
            }
        }
        if (sep == NONE) {
            if (!seq) return false;
            std::uint32_t ve = comment != NONE ? comment : end;
            while (ve > p && t[ve - 1] == ' ') --ve;
            pt.kind = LineKind::SeqItem;
            pt.val_s = p;
            pt.val_e = ve;
            if (comment != NONE) pt.flags |= line_flag::COMMENT;
            return true;
        }
        std::uint32_t ke = sep;
        while (ke > p && t[ke - 1] == ' ') --ke;
        for (std::uint32_t k = p; k < ke; ++k)
            if (is_flow_char(t[k])) return false;
        if (is_null_key(dstd::StringView(t + p, ke - p))) return false;
        pt.key_s = p;
        pt.key_e = ke;
        val_from = sep + 1;
    }
    pt.kind = seq ? LineKind::SeqMap : LineKind::MapEntry;
    std::uint32_t v = val_from;
    while (v < end && t[v] == ' ') ++v;
    if (v == end) {
        pt.flags |= line_flag::VAL_EMPTY;
        return true;
    }
    if (t[v] == '#') {
        pt.flags |= line_flag::VAL_EMPTY | line_flag::COMMENT;
        return true;
    }
    const char vc = t[v];
    if (vc == '"' || vc == '\'') {
        Quoted q;
        if (!scan_quoted(t, v, end, q))
            return unclosed(t, v, end, text_size, pt);
        bool has_comment = false;
        std::uint32_t stray = 0;
        if (!ends_in_comment_or_nothing(t, q.after, end, has_comment, stray)) {
            if (dstd::is_ascii_alnum(t[stray])) {
                pt.error = LineError::TrailingText;
                pt.error_at = stray;
            }
            return false;
        }
        pt.val_s = v + 1;
        pt.val_e = q.inner_end;
        pt.flags |= line_flag::VAL_QUOTED;
        if (q.esc) pt.flags |= line_flag::VAL_ESC;
        if (has_comment) pt.flags |= line_flag::COMMENT;
        return true;
    }
    if (vc == '[' || vc == '{') {
        bool has_comment = false;
        if (!is_empty_flow(t, v, end, has_comment)) return false;
        pt.val_s = v;
        pt.val_e = v + 2;
        pt.flags |= line_flag::FLOW_EMPTY;
        if (has_comment) pt.flags |= line_flag::COMMENT;
        return true;
    }
    if (is_bad_lead(vc)) return false;
    if (vc == '-' && (v + 1 == end || t[v + 1] == ' ')) return false;
    std::uint32_t comment = NONE;
    for (std::uint32_t k = v; k < end; ++k) {
        const char ch = t[k];
        if (ch == ':' && (k + 1 == end || t[k + 1] == ' ')) {
            pt.error = LineError::MapValue;
            pt.error_at = k;
            return false;
        }
        if (ch == ' ' && k + 1 < end && t[k + 1] == '#') {
            comment = k;
            break;
        }
    }
    std::uint32_t ve = comment != NONE ? comment : end;
    while (ve > v && t[ve - 1] == ' ') --ve;
    pt.val_s = v;
    pt.val_e = ve;
    if (comment != NONE) pt.flags |= line_flag::COMMENT;
    return true;
}

namespace {

void append_utf8(dstd::Vector<char>& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

int hex_value(char c) {
    if (dstd::is_ascii_digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

struct EscapeProblem {
    enum Kind : std::uint8_t { None, Unknown, Digits, Hex, Range } kind = None;
    std::uint32_t at = 0;
    char ch = 0;
    std::uint32_t count = 0;
};

enum class Decoded : std::uint8_t { Ok, Decline, Invalid };

// Appends the text of the quoted scalar whose inner bytes are [s, e). A valid
// escape that the line index does not take declines, as the tape reader folds
// some of them in its own way; an invalid one is a problem.
Decoded decode_quoted(const char* t, std::uint32_t s, std::uint32_t e,
                      bool double_quoted, dstd::Vector<char>& out,
                      EscapeProblem& problem) {
    if (!double_quoted) {
        for (std::uint32_t i = s; i < e; ++i) {
            out.push_back(t[i]);
            if (t[i] == '\'') ++i;
        }
        return Decoded::Ok;
    }
    for (std::uint32_t i = s; i < e; ++i) {
        if (t[i] != '\\') {
            out.push_back(t[i]);
            continue;
        }
        const std::uint32_t at = i;
        ++i;
        std::uint32_t cp = 0;
        switch (t[i]) {
            case '\\':
            case '"':
            case '/':
                out.push_back(t[i]);
                continue;
            case 'n':
                out.push_back('\n');
                continue;
            case 't':
                out.push_back('\t');
                continue;
            case 'r':
                out.push_back('\r');
                continue;
            case 'b':
                out.push_back('\b');
                continue;
            case 'f':
                out.push_back('\f');
                continue;
            case 'a':
                out.push_back('\a');
                continue;
            case 'e':
                out.push_back('\x1B');
                continue;
            case 'v':
                out.push_back('\v');
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
                    problem = {EscapeProblem::Digits, at, t[i], digits};
                    return Decoded::Invalid;
                }
                for (std::uint32_t d = 0; d < digits; ++d) {
                    const int h = hex_value(t[i + 1 + d]);
                    if (h < 0) {
                        problem = {EscapeProblem::Hex, at, t[i], digits};
                        return Decoded::Invalid;
                    }
                    cp = (cp << 4) | static_cast<std::uint32_t>(h);
                }
                i += digits;
                if (cp > 0x10FFFF) {
                    problem = {EscapeProblem::Range, at, t[i - digits], digits};
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
                problem = {EscapeProblem::Unknown, at, t[i], 0};
                return Decoded::Invalid;
        }
        append_utf8(out, cp);
    }
    return Decoded::Ok;
}

// A 32-bit hash of a key from its first and last 8 bytes and its length, which
// are the whole key up to 16 bytes. `limit` ends the text: a load of 8 bytes
// stays inside it.
std::uint32_t hash_key(const char* p, std::size_t n, const char* limit) {
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    if (n >= 8) {
        std::memcpy(&first, p, 8);
        std::memcpy(&last, p + n - 8, 8);
    } else if (n != 0) {
        if (p + 8 <= limit) {
            std::memcpy(&first, p, 8);
            first &= (std::uint64_t{1} << (8 * n)) - 1;
        } else {
            std::memcpy(&first, p, n);
        }
        last = first;
    }
    std::uint64_t h = first * 0x9E3779B97F4A7C15ull;
    h ^= (last + n) * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 32;
    return static_cast<std::uint32_t>(h);
}

// The sequences of 0x80 and above that a YAML reader takes for a break, or
// refuses.
bool has_bad_utf8(const char* t, std::uint32_t from, std::uint32_t to,
                  std::uint32_t size) {
    for (std::uint32_t i = from; i < to; ++i) {
        const auto c = static_cast<unsigned char>(t[i]);
        if (c < 0x80) continue;
        const auto n1 = i + 1 < size ? static_cast<unsigned char>(t[i + 1]) : 0;
        const auto n2 = i + 2 < size ? static_cast<unsigned char>(t[i + 2]) : 0;
        if (c == 0xC2 && n1 >= 0x80 && n1 <= 0xA0) return true;
        if (c == 0xE2 && n1 == 0x80 && (n2 == 0xA8 || n2 == 0xA9)) return true;
        if (c == 0xEF && n1 == 0xBB && n2 == 0xBF) return true;
        if (c == 0xEF && n1 == 0xBF && (n2 == 0xBE || n2 == 0xBF)) return true;
    }
    return false;
}

// The bit arrays of one run of blocks, positions counted from `base`.
struct Batch {
    const std::uint64_t* space;
    const std::uint64_t* sep;
    const std::uint64_t* special;
    std::uint32_t base;
    std::uint32_t bits;
};

// The 64 bits of `a` that start at bit `pos`. The array has a zero word after
// its last block.
inline std::uint64_t window(const std::uint64_t* a, std::uint32_t pos) {
    const std::uint32_t w = pos >> 6;
    const std::uint32_t sh = pos & 63;
    return (a[w] >> sh) | ((a[w + 1] << 1) << (63 - sh));
}

// The first set bit at or after `pos`, or a position at or past `limit` when
// there is none before it.
inline std::uint32_t next_set(const std::uint64_t* a, std::uint32_t limit,
                              std::uint32_t pos) {
    std::uint64_t x = window(a, pos);
    if (x != 0) [[likely]]
        return pos + static_cast<std::uint32_t>(dstd::bits::ctz_u64(x));
    for (pos += 64; pos < limit; pos += 64) {
        x = window(a, pos);
        if (x != 0)
            return pos + static_cast<std::uint32_t>(dstd::bits::ctz_u64(x));
    }
    return limit;
}

// The first clear bit at or after `pos`, or a position at or past `limit`.
inline std::uint32_t next_clear(const std::uint64_t* a, std::uint32_t limit,
                                std::uint32_t pos) {
    std::uint64_t x = ~window(a, pos);
    if (x != 0) [[likely]]
        return pos + static_cast<std::uint32_t>(dstd::bits::ctz_u64(x));
    for (pos += 64; pos < limit; pos += 64) {
        x = ~window(a, pos);
        if (x != 0)
            return pos + static_cast<std::uint32_t>(dstd::bits::ctz_u64(x));
    }
    return limit;
}

// The line `[start, end)` when it holds none of the bytes that set a line
// apart from a plain one: the same facts as classify_line, read from the bit
// arrays. False means the line goes to classify_line, whatever it holds.
bool classify_plain(const Batch& b, const char* t, std::uint32_t start,
                    std::uint32_t end, LineParts& pt) {
    const std::uint32_t rs = start - b.base;
    const std::uint32_t re = end - b.base;
    if (next_set(b.special, re, rs) < re) return false;
    const std::uint32_t ri = next_clear(b.space, re, rs);
    const std::uint32_t i = b.base + ri;
    pt.indent = i - start;
    pt.flags = 0;
    if (pt.indent > INDENT_LIMIT) return false;
    if (i >= end) {
        pt.kind = LineKind::Blank;
        return true;
    }
    const char c = t[i];
    if (c == ':') return false;
    if (pt.indent == 0 && (c == '-' || c == '.') && end - i >= 3 &&
        t[i + 1] == c && t[i + 2] == c)
        return false;
    bool seq = false;
    std::uint32_t p = i;
    std::uint32_t rp = ri;
    if (c == '-' && (i + 1 == end || t[i + 1] == ' ')) {
        seq = true;
        rp = next_clear(b.space, re, ri + 1);
        p = b.base + rp;
        pt.kind = LineKind::SeqItem;
        pt.content = p;
        if (p >= end) {
            pt.flags = line_flag::VAL_EMPTY;
            return true;
        }
        if (t[p] == ':') return false;
        if (t[p] == '-' && (p + 1 == end || t[p + 1] == ' ')) return false;
    }
    pt.content = p;
    const std::uint32_t rsep = next_set(b.sep, re, rp);
    if (rsep >= re) {
        if (!seq) return false;
        std::uint32_t ve = end;
        while (ve > p && t[ve - 1] == ' ') --ve;
        pt.kind = LineKind::SeqItem;
        pt.val_s = p;
        pt.val_e = ve;
        return true;
    }
    const std::uint32_t sep = b.base + rsep;
    std::uint32_t ke = sep;
    while (ke > p && t[ke - 1] == ' ') --ke;
    if (ke - p <= 4 && is_null_key(dstd::StringView(t + p, ke - p)))
        return false;
    pt.key_s = p;
    pt.key_e = ke;
    pt.kind = seq ? LineKind::SeqMap : LineKind::MapEntry;
    const std::uint32_t rv = next_clear(b.space, re, rsep + 1);
    const std::uint32_t v = b.base + rv;
    if (v >= end) {
        pt.flags = line_flag::VAL_EMPTY;
        return true;
    }
    if (t[v] == ':' || (t[v] == '-' && (v + 1 == end || t[v + 1] == ' ')))
        return false;
    if (next_set(b.sep, re, rv) < re) return false;
    std::uint32_t ve = end;
    while (ve > v && t[ve - 1] == ' ') --ve;
    pt.val_s = v;
    pt.val_e = ve;
    return true;
}

std::atomic<bool> g_slow_lines{false};

struct Level {
    std::uint64_t bloom[4];
    std::uint32_t cur;
    std::uint32_t key_base;
    std::uint32_t key_count;
    std::uint16_t col;
    bool is_seq;
    bool has_table;
    /// A sequence that sits at the column of the map that holds its key.
    bool inline_seq;
};

class Builder {
   public:
    explicit Builder(LineIndex& ix) : ix_(ix), t_(ix.text) {}

    bool build(std::size_t size);
    bool is_invalid() const { return invalid_; }

   private:
    bool process_line(std::uint32_t start, std::uint32_t nl_pos);
    const Batch* batch_ = nullptr;
    bool structure(std::uint32_t j, const LineParts& pt);
    bool marker(std::uint32_t j, LineKind kind);
    bool push_level(std::uint32_t j, std::uint32_t col, bool is_seq);
    void pop_level(std::uint32_t at);
    bool note_key(Level& level, std::uint32_t line, dstd::StringView key);
    bool table_insert(Level& level, std::uint32_t hash, std::uint32_t line,
                      dstd::StringView key);
    bool same_key(std::uint32_t line, dstd::StringView key) const;
    bool decode_into(std::uint32_t line, bool is_value, std::uint32_t s,
                     std::uint32_t e);
    bool fail(std::uint32_t line, std::uint32_t offset, dstd::String reason);
    dstd::StringView key_view(std::uint32_t line, const LineParts& pt) const;
    void resolve_pending(bool child, std::uint32_t at);

    LineIndex& ix_;
    const char* t_;
    std::uint32_t line_count_ = 0;
    std::uint32_t line_ = 0;
    Level levels_[MAX_DEPTH];
    std::size_t depth_ = 0;
    /// The keys of the open maps: the hash, then the line.
    dstd::RawBuffer<std::uint64_t> keys_;
    dstd::Vector<std::uint64_t> tables_[MAX_DEPTH];
    std::uint32_t pending_ = NONE;
    std::uint32_t pending_col_ = 0;
    bool cont_plain_ = false;
    bool comment_seen_ = false;
    bool cont_quoted_ = false;
    bool cont_strict_ = false;
    bool cont_active_ = false;
    bool invalid_ = false;
    std::uint32_t cont_limit_ = 0;
    bool explicit_open_ = false;
    bool has_content_ = false;
    bool after_end_ = false;
    bool any_content_ = false;
};

dstd::StringView Builder::key_view(std::uint32_t line,
                                   const LineParts& pt) const {
    if ((pt.flags & line_flag::KEY_ESC) != 0) {
        const std::uint32_t key = line * 2;
        const auto it = std::lower_bound(
            ix_.escapes.begin(), ix_.escapes.end(), key,
            [](const EscapeEntry& e, std::uint32_t k) { return e.key < k; });
        return dstd::StringView(ix_.arena.data() + it->off, it->len);
    }
    return dstd::StringView(t_ + pt.key_s, pt.key_e - pt.key_s);
}

bool Builder::same_key(std::uint32_t line, dstd::StringView key) const {
    LineParts pt;
    if (!classify_line(t_, ix_.entries[line].start, ix_.line_end(line),
                       ix_.size, pt))
        return false;
    return key_view(line, pt) == key;
}

bool Builder::fail(std::uint32_t line, std::uint32_t offset,
                   dstd::String reason) {
    ix_.problem.line = line + 1;
    ix_.problem.col = offset - ix_.entries[line].start + 1;
    ix_.problem.reason = std::move(reason);
    invalid_ = true;
    return false;
}

bool Builder::decode_into(std::uint32_t line, bool is_value, std::uint32_t s,
                          std::uint32_t e) {
    const bool dq = t_[s - 1] == '"';
    const auto off = static_cast<std::uint32_t>(ix_.arena.size());
    EscapeProblem problem;
    const Decoded decoded = decode_quoted(t_, s, e, dq, ix_.arena, problem);
    if (decoded == Decoded::Decline) return false;
    if (decoded == Decoded::Invalid) {
        switch (problem.kind) {
            case EscapeProblem::Unknown:
                return fail(line, problem.at,
                            dstd::fmt<"unknown escape character '{}'">(
                                dstd::StringView(&problem.ch, 1)));
            case EscapeProblem::Digits:
                return fail(
                    line, problem.at,
                    dstd::fmt<"escape '\\{}' needs {} hex digits">(
                        dstd::StringView(&problem.ch, 1), problem.count));
            case EscapeProblem::Hex:
                return fail(
                    line, problem.at,
                    dstd::fmt<"escape '\\{}' has a character that is not a hex "
                              "digit">(dstd::StringView(&problem.ch, 1)));
            default:
                return fail(line, problem.at, "escape is not a code point");
        }
    }
    const auto len = static_cast<std::uint32_t>(ix_.arena.size()) - off;
    ix_.escapes.push_back(
        EscapeEntry{line * 2 + (is_value ? 1u : 0u), off, len});
    return true;
}

bool Builder::table_insert(Level& level, std::uint32_t hash, std::uint32_t line,
                           dstd::StringView key) {
    dstd::Vector<std::uint64_t>& table = tables_[&level - levels_];
    const std::size_t want =
        (static_cast<std::size_t>(level.key_count) + 1) * 2;
    if (table.size() < want) {
        std::size_t n = std::max<std::size_t>(table.size() * 2, 256);
        while (n < want) n *= 2;
        table.assign(n, 0);
        const std::size_t mask = n - 1;
        for (std::size_t i = level.key_base; i < keys_.size(); ++i) {
            const auto entry_hash = static_cast<std::uint32_t>(keys_[i] >> 32);
            std::size_t slot = entry_hash & mask;
            while (table[slot] != 0) slot = (slot + 1) & mask;
            table[slot] = (static_cast<std::uint64_t>(entry_hash) << 32) |
                          ((keys_[i] & 0xFFFFFFFFull) + 1ull);
        }
        level.has_table = true;
    }
    const std::size_t mask = table.size() - 1;
    std::size_t slot = hash & mask;
    while (table[slot] != 0) {
        if ((table[slot] >> 32) == hash &&
            same_key(static_cast<std::uint32_t>(table[slot]) - 1, key))
            return false;
        slot = (slot + 1) & mask;
    }
    table[slot] = (static_cast<std::uint64_t>(hash) << 32) | (line + 1ull);
    return true;
}

// False when the map holds the key already. The lists hold the keys before it.
bool Builder::note_key(Level& level, std::uint32_t line, dstd::StringView key) {
    const bool in_text = key.data() >= t_ && key.data() < t_ + ix_.size;
    const std::uint32_t hash =
        hash_key(key.data(), key.size(), in_text ? t_ + ix_.size : key.data());
    const std::uint32_t index = level.key_count;
    if (index >= LINEAR_KEY_LIMIT) {
        if (!table_insert(level, hash, line, key)) return false;
    } else {
        std::uint64_t& word = level.bloom[(hash >> 6) & 3];
        const std::uint64_t bit = std::uint64_t{1} << (hash & 63);
        if ((word & bit) != 0) {
            for (std::uint32_t i = 0; i < index; ++i) {
                const std::uint64_t entry = keys_[level.key_base + i];
                if (static_cast<std::uint32_t>(entry >> 32) == hash &&
                    same_key(static_cast<std::uint32_t>(entry), key))
                    return false;
            }
        }
        word |= bit;
    }
    ++level.key_count;
    keys_.push_back((static_cast<std::uint64_t>(hash) << 32) | line);
    return true;
}

bool Builder::push_level(std::uint32_t j, std::uint32_t col, bool is_seq) {
    if (depth_ == MAX_DEPTH) return false;
    Level& level = levels_[depth_++];
    level.bloom[0] = level.bloom[1] = level.bloom[2] = level.bloom[3] = 0;
    level.cur = j;
    level.key_base = static_cast<std::uint32_t>(keys_.size());
    level.key_count = 0;
    level.col = static_cast<std::uint16_t>(col);
    level.is_seq = is_seq;
    level.has_table = false;
    level.inline_seq = false;
    return true;
}

void Builder::pop_level(std::uint32_t at) {
    Level& level = levels_[depth_ - 1];
    if (level.cur != NONE) ix_.next[level.cur] = at;
    if (!level.is_seq) {
        keys_.resize(level.key_base);
        if (level.has_table) tables_[depth_ - 1].clear();
    }
    --depth_;
}

void Builder::resolve_pending(bool child, std::uint32_t) {
    if (pending_ == NONE) return;
    if (child) {
        LineEntry& e = ix_.entries[pending_];
        switch (e.kind) {
            case LineKind::MapEntry:
                e.kind = LineKind::MapOpen;
                break;
            case LineKind::SeqItem:
                e.kind = LineKind::SeqOpen;
                break;
            case LineKind::SeqMap:
                e.kind = LineKind::SeqMapOpen;
                break;
            default:
                break;
        }
    }
    pending_ = NONE;
}

bool Builder::marker(std::uint32_t j, LineKind kind) {
    resolve_pending(false, j);
    while (depth_ > 0) pop_level(j);
    cont_active_ = false;
    ix_.has_markers = true;
    if (kind == LineKind::DocStart) {
        if (explicit_open_ && !has_content_) return false;
        explicit_open_ = true;
        has_content_ = false;
        after_end_ = false;
    } else {
        if (!has_content_) return false;
        has_content_ = false;
        explicit_open_ = false;
        after_end_ = true;
    }
    return true;
}

bool Builder::structure(std::uint32_t j, const LineParts& pt) {
    if (after_end_) return false;
    has_content_ = true;
    any_content_ = true;
    const std::uint32_t n = pt.indent;
    if (cont_active_ && n > cont_limit_) {
        // A plain value goes on in a deeper line, which the line index
        // declines, unless the line holds a key, or the value is complete.
        const bool has_key =
            pt.kind == LineKind::MapEntry || pt.kind == LineKind::SeqMap;
        if (cont_quoted_) {
            if (cont_strict_)
                return fail(j, ix_.entries[j].start + n,
                            "a complete value cannot go on in a deeper line");
            return false;
        }
        if (has_key && cont_plain_ && !comment_seen_)
            return fail(j, pt.content,
                        "mapping values are not allowed in a multi-line plain "
                        "scalar");
        return false;
    }
    cont_active_ = false;
    comment_seen_ = false;
    const bool is_seq =
        pt.kind == LineKind::SeqItem || pt.kind == LineKind::SeqMap;
    const bool child = pending_ != NONE && n > pending_col_;
    bool inline_seq = false;
    if (pending_ != NONE && n == pending_col_ && is_seq) {
        const LineKind pending_kind = ix_.entries[pending_].kind;
        inline_seq = pending_kind == LineKind::MapEntry ||
                     pending_kind == LineKind::SeqMap;
    }
    resolve_pending(child || inline_seq, j);
    if (depth_ == 0) {
        if (n != 0) return false;
        if (!push_level(j, n, is_seq)) return false;
    } else if (child) {
        if (!push_level(j, n, is_seq)) return false;
    } else if (inline_seq) {
        if (!push_level(j, n, true)) return false;
        levels_[depth_ - 1].inline_seq = true;
    } else {
        while (depth_ > 0 && levels_[depth_ - 1].col > n) pop_level(j);
        if (depth_ > 1 && levels_[depth_ - 1].inline_seq &&
            levels_[depth_ - 1].col == n && !is_seq) {
            // The tape reader refuses a key that starts like an item or a
            // marker right after such a sequence.
            const char lead = t_[ix_.entries[j].start + n];
            if (lead == '-' || lead == '.') return false;
            pop_level(j);
        }
        if (depth_ == 0 || levels_[depth_ - 1].col != n)
            return fail(j, ix_.entries[j].start + n,
                        "indentation does not match any open block");
        Level& top = levels_[depth_ - 1];
        if (top.is_seq != is_seq)
            return fail(j, ix_.entries[j].start + n,
                        is_seq ? "a sequence item cannot follow mapping keys "
                                 "at the same indentation"
                               : "a mapping key cannot follow sequence items "
                                 "at the same indentation");
        if (top.cur != NONE) ix_.next[top.cur] = j;
        top.cur = j;
    }
    std::uint32_t map_col = n;
    if (pt.kind == LineKind::SeqMap) {
        map_col = pt.content - ix_.entries[j].start;
        if (map_col > INDENT_LIMIT) return false;
        if (!push_level(NONE, map_col, false)) return false;
    }
    if (pt.kind == LineKind::MapEntry || pt.kind == LineKind::SeqMap) {
        Level& level = levels_[depth_ - 1];
        const dstd::StringView key = key_view(j, pt);
        if (!note_key(level, j, key)) {
            const std::uint32_t quoted =
                (pt.flags & line_flag::KEY_QUOTED) != 0 ? 1u : 0u;
            return fail(j, pt.key_s - quoted,
                        dstd::fmt<"duplicate key '{}'">(key));
        }
    }
    if (pt.kind == LineKind::MapEntry || pt.kind == LineKind::SeqMap ||
        pt.kind == LineKind::SeqItem) {
        const std::uint32_t limit = pt.kind == LineKind::SeqMap ? map_col : n;
        if ((pt.flags & line_flag::VAL_EMPTY) != 0) {
            pending_ = j;
            pending_col_ = limit;
        } else {
            cont_active_ = true;
            cont_limit_ = limit;
            // The tape reader reads a deeper line after a quoted item in
            // ways of its own, so only a keyed value is complete.
            cont_plain_ =
                (pt.flags & (line_flag::VAL_QUOTED | line_flag::FLOW_EMPTY |
                             line_flag::COMMENT)) == 0;
            cont_quoted_ =
                (pt.flags & (line_flag::VAL_QUOTED | line_flag::FLOW_EMPTY)) !=
                0;
            cont_strict_ = pt.kind != LineKind::SeqItem;
        }
    }
    return true;
}

bool Builder::process_line(std::uint32_t start, std::uint32_t nl_pos) {
    const std::uint32_t j = line_++;
    std::uint32_t end = nl_pos;
    std::uint8_t flags = 0;
    if (end > start && t_[end - 1] == '\r') {
        --end;
        flags = line_flag::CR;
    }
    LineEntry& entry = ix_.entries[j];
    entry.start = start;
    ix_.entries[j + 1].start = nl_pos + 1;
    ix_.next[j] = line_count_;
    LineParts pt;
    const bool plain = batch_ != nullptr && start >= batch_->base &&
                       classify_plain(*batch_, t_, start, end, pt);
    if (!plain && !classify_line(t_, start, end, ix_.size, pt)) {
        // A line that goes on a plain value of the line before may hold any
        // text, quotes included.
        if (cont_active_ && pt.indent > cont_limit_) {
            if (cont_quoted_ && cont_strict_ && pt.error == LineError::NONE)
                return fail(j, start + pt.indent,
                            "a complete value cannot go on in a deeper line");
            return false;
        }
        switch (pt.error) {
            case LineError::Unclosed:
                return fail(j, pt.error_at, "unterminated quoted scalar");
            case LineError::TrailingText:
                return fail(j, pt.error_at,
                            "unexpected text after a quoted scalar");
            case LineError::MapValue:
                return fail(j, pt.error_at,
                            "mapping values are not allowed in this context");
            default:
                return false;
        }
    }
    entry.indent = static_cast<std::uint16_t>(pt.indent);
    entry.kind = pt.kind;
    entry.flags = static_cast<std::uint8_t>(flags | pt.flags);
    switch (pt.kind) {
        case LineKind::Blank:
            return true;
        case LineKind::Comment:
            comment_seen_ = true;
            return true;
        case LineKind::DocStart:
        case LineKind::DocEnd:
            return marker(j, pt.kind);
        default:
            break;
    }
    // A line that goes on a value is text, not a scalar to decode.
    if (cont_active_ && pt.indent > cont_limit_) return structure(j, pt);
    if ((pt.flags & line_flag::KEY_ESC) != 0 &&
        !decode_into(j, false, pt.key_s, pt.key_e))
        return false;
    if ((pt.flags & line_flag::VAL_ESC) != 0 &&
        !decode_into(j, true, pt.val_s, pt.val_e))
        return false;
    return structure(j, pt);
}

bool Builder::build(std::size_t size) {
    const auto text_size = static_cast<std::uint32_t>(size);
    const bool ends_in_newline = t_[size - 1] == '\n';
    line_count_ = static_cast<std::uint32_t>(detail::count_newlines(t_, size) +
                                             (ends_in_newline ? 0 : 1));
    if (line_count_ > size / MIN_AVERAGE_LINE) return false;
    ix_.entries.reserve(line_count_ + 1);
    ix_.entries.resize(line_count_ + 1);
    ix_.next.reserve(line_count_ + 1);
    ix_.next.resize(line_count_ + 1);
    ix_.line_count = line_count_;
    ix_.next[line_count_] = line_count_;
    std::uint32_t line_start = 0;
    std::uint64_t nl[BATCH_BLOCKS];
    std::uint64_t bad[BATCH_BLOCKS];
    std::uint64_t high[BATCH_BLOCKS];
    std::uint64_t space[BATCH_BLOCKS + 1];
    std::uint64_t sep[BATCH_BLOCKS + 1];
    std::uint64_t special[BATCH_BLOCKS + 1];
    const detail::MaskArrays arrays{nl, bad, high, space, sep, special};
    std::uint32_t pos[BATCH_BLOCKS * 64 + 8];
    alignas(64) char tail[192];
    const bool slow = g_slow_lines.load();
    std::size_t off = 0;
    for (;;) {
        std::size_t blocks = 0;
        const char* source = t_ + off;
        bool is_tail = false;
        if (off + 64 < size) {
            blocks = std::min<std::size_t>(BATCH_BLOCKS, (size - off - 1) / 64);
        } else {
            const std::size_t rem = size - off;
            if (rem == 0) break;
            std::memset(tail, ' ', sizeof(tail));
            std::memcpy(tail, t_ + off, rem);
            source = tail;
            blocks = 1;
            is_tail = true;
        }
        detail::line_masks(source, blocks, arrays);
        space[blocks] = sep[blocks] = special[blocks] = 0;
        std::uint64_t any_bad = 0;
        std::size_t total = 0;
        for (std::size_t b = 0; b < blocks; ++b) {
            any_bad |= bad[b];
            const auto base = static_cast<std::uint32_t>(off + b * 64);
            if (high[b] != 0) {
                const auto to = static_cast<std::uint32_t>(
                    std::min<std::size_t>(base + 64, size));
                if (has_bad_utf8(t_, base, to, text_size)) return false;
            }
            std::uint64_t bits = nl[b];
            const int count = dstd::bits::popcount_u64(bits);
            std::uint32_t* out = pos + total;
            for (int w = 0; w < 8; ++w) {
                out[w] = base +
                         static_cast<std::uint32_t>(dstd::bits::ctz_u64(bits));
                bits &= bits - 1;
            }
            for (int w = 8; w < count; ++w) {
                out[w] = base +
                         static_cast<std::uint32_t>(dstd::bits::ctz_u64(bits));
                bits &= bits - 1;
            }
            total += static_cast<std::size_t>(count);
        }
        if (any_bad != 0) return false;
        const Batch batch{space, sep, special, static_cast<std::uint32_t>(off),
                          static_cast<std::uint32_t>(blocks * 64)};
        batch_ = slow ? nullptr : &batch;
        for (std::size_t k = 0; k < total; ++k) {
            if (!process_line(line_start, pos[k])) return false;
            line_start = pos[k] + 1;
        }
        batch_ = nullptr;
        if (is_tail) break;
        off += blocks * 64;
    }
    if (line_start < text_size) {
        if (!process_line(line_start, text_size)) return false;
    }
    if (line_ != line_count_) return false;
    if (explicit_open_ && !has_content_) return false;
    resolve_pending(false, line_count_);
    ix_.entries[line_count_].indent = 0;
    ix_.entries[line_count_].kind = LineKind::Blank;
    ix_.entries[line_count_].flags = 0;
    return any_content_;
}

}  // namespace

void LineIndex::release() {
    entries.release();
    next.release();
    arena = dstd::Vector<char>();
    escapes = dstd::Vector<EscapeEntry>();
    line_count = 0;
}

dstd::Unexpected<dstd::Error> line_problem_error(const LineProblem& problem,
                                                 dstd::StringView name,
                                                 std::uint64_t first_line) {
    const std::uint64_t line = first_line + problem.line - 1;
    if (name.empty())
        return common::make_error<"{}:{}: {}">(
            common::ErrorCode::InvalidArgument, line, problem.col,
            problem.reason);
    return common::make_error<"{}:{}:{}: {}">(
        common::ErrorCode::InvalidArgument, name, line, problem.col,
        problem.reason);
}

namespace detail {
void set_slow_lines_for_test(bool slow) { g_slow_lines.store(slow); }
}  // namespace detail

BuildResult LineIndex::build(const char* text_in, std::size_t size_in) {
    text = text_in;
    size = 0;
    line_count = 0;
    has_markers = false;
    arena.clear();
    escapes.clear();
    entries.clear();
    next.clear();
    problem = LineProblem{};
    if (size_in == 0 || size_in >= TEXT_SIZE_LIMIT)
        return BuildResult::Declined;
    size = static_cast<std::uint32_t>(size_in);
    Builder builder(*this);
    if (builder.build(size_in)) return BuildResult::Accepted;
    return builder.is_invalid() ? BuildResult::Invalid : BuildResult::Declined;
}

}  // namespace dftracer::utils::yaml
