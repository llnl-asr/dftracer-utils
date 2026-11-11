// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/bits.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/yaml/line_index.h>
#include <dftracer/utils/yaml/line_scalar.h>
#include <dftracer/utils/yaml/scalar.h>

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
/// Only a text of at least this size is left to the tape reader for its short
/// lines: a smaller one costs the index nothing.
constexpr std::size_t SHORT_LINE_CHECK_SIZE = 4096;

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

// The quote that closes the scalar quoted with `quote` and goes on at `from`,
// or NONE when the line does not close it.
std::uint32_t find_close(const char* t, std::uint32_t from, std::uint32_t end,
                         char quote) {
    for (std::uint32_t i = from; i < end; ++i) {
        const char c = t[i];
        if (quote == '"') {
            if (c == '\\') {
                ++i;
            } else if (c == '"') {
                return i;
            }
        } else if (c == '\'') {
            if (i + 1 < end && t[i + 1] == '\'') {
                ++i;
            } else {
                return i;
            }
        }
    }
    return NONE;
}

// A quoted scalar that the line does not close.
void open_quote(LineParts& pt, std::uint32_t quote_at, std::uint32_t end) {
    pt.open = LineOpen::Quoted;
    pt.open_at = quote_at;
    pt.val_s = quote_at + 1;
    pt.val_e = end;
    pt.flags |= line_flag::VAL_QUOTED | line_flag::VAL_ESC;
}

// Whether rapidyaml refuses the text of a block scalar header after its `|` or
// `>`, which lies in `[from, end)`: a model of its header parse in
// `src/c4/yml/parse_engine.def.hpp` (MIT,
// https://github.com/biojppm/rapidyaml), so that an error is reported only
// where it reports one. It looks for the first `-` or `+` anywhere in the line,
// drops what follows it, then reads a run of digits and ignores the rest, or
// else takes anything but a comment for an error.
bool refuses_header(const char* t, std::uint32_t from, std::uint32_t end) {
    std::uint32_t b = from;
    std::uint32_t e = end;
    for (std::uint32_t i = from; i < end; ++i) {
        if (t[i] != '-' && t[i] != '+') continue;
        if (i == from) {
            b = from + 1;
        } else {
            e = i;
        }
        break;
    }
    std::uint32_t d = b;
    while (d < e && dstd::is_ascii_digit(t[d])) ++d;
    if (d > b) return d - b > 1 || t[b] == '0';
    std::uint32_t r = b;
    while (r < e && (t[r] == ' ' || t[r] == '\t')) ++r;
    return r < e && (t[r] != '#' || t[b] == '#');
}

// The header of the block scalar whose `|` or `>` is at `p`: an indentation
// indicator and a chomping indicator in either order, then nothing or a
// comment set off by a space. Text that the YAML spec refuses is an error
// where rapidyaml refuses it too, and else outside the subset.
bool classify_block_header(const char* t, std::uint32_t p, std::uint32_t end,
                           LineParts& pt) {
    std::uint32_t i = p + 1;
    bool has_digit = false;
    bool has_chomp = false;
    for (; i < end; ++i) {
        const char c = t[i];
        if (c >= '1' && c <= '9' && !has_digit) {
            has_digit = true;
        } else if ((c == '+' || c == '-') && !has_chomp) {
            has_chomp = true;
        } else {
            break;
        }
    }
    std::uint32_t a = i;
    while (a < end && t[a] == ' ') ++a;
    if (a < end && !(t[a] == '#' && a > i)) {
        if (refuses_header(t, p + 1, end)) {
            pt.error = LineError::BlockHeader;
            pt.error_at = a;
        }
        return false;
    }
    // rapidyaml takes a `-` or `+` of the comment for an indicator.
    if (!has_chomp) {
        for (std::uint32_t k = a; k < end; ++k)
            if (t[k] == '-' || t[k] == '+') return false;
    }
    pt.open = LineOpen::Block;
    pt.open_at = p;
    pt.val_s = p;
    pt.val_e = i;
    pt.flags |= line_flag::VAL_ESC;
    if (a < end) pt.flags |= line_flag::COMMENT;
    return true;
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

namespace {

// The top-level scalar that follows `---` on the line, which starts at `k`.
bool classify_root_doc(const char* t, std::uint32_t k, std::uint32_t end,
                       LineParts& pt) {
    pt.kind = LineKind::RootDoc;
    pt.content = k;
    const char c = t[k];
    if (c == '|' || c == '>') return classify_block_header(t, k, end, pt);
    if (c == '"' || c == '\'') {
        Quoted q;
        if (!scan_quoted(t, k, end, q)) {
            open_quote(pt, k, end);
            return true;
        }
        bool has_comment = false;
        std::uint32_t stray = 0;
        if (!ends_in_comment_or_nothing(t, q.after, end, has_comment, stray))
            return false;
        pt.val_s = k + 1;
        pt.val_e = q.inner_end;
        pt.flags |= line_flag::VAL_QUOTED;
        if (q.esc) pt.flags |= line_flag::VAL_ESC;
        if (has_comment) pt.flags |= line_flag::COMMENT;
        return true;
    }
    if (c == '[' || c == '{' || is_bad_lead(c)) return false;
    if (c == '-' && (k + 1 == end || t[k + 1] == ' ')) return false;
    std::uint32_t comment = NONE;
    for (std::uint32_t i = k; i < end; ++i) {
        const char ch = t[i];
        if (ch == ':' && (i + 1 == end || t[i + 1] == ' ')) return false;
        if (ch == ' ' && i + 1 < end && t[i + 1] == '#') {
            comment = i;
            break;
        }
    }
    std::uint32_t ve = comment != NONE ? comment : end;
    while (ve > k && t[ve - 1] == ' ') --ve;
    pt.val_s = k;
    pt.val_e = ve;
    if (comment != NONE) pt.flags |= line_flag::COMMENT;
    return true;
}

}  // namespace

bool classify_line(const char* t, std::uint32_t start, std::uint32_t end,
                   LineParts& pt) {
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
        if (k < end && t[k] != '#') {
            if (t[i] != '-') return false;
            return classify_root_doc(t, k, end, pt);
        }
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
    if (c == '|' || c == '>') {
        pt.kind = seq ? LineKind::SeqItem : LineKind::Root;
        return classify_block_header(t, p, end, pt);
    }
    if (c == '"' || c == '\'') {
        Quoted q;
        if (!scan_quoted(t, p, end, q)) {
            pt.kind = seq ? LineKind::SeqItem : LineKind::Root;
            open_quote(pt, p, end);
            return true;
        }
        std::uint32_t a = q.after;
        while (a < end && t[a] == ' ') ++a;
        if (a < end && t[a] == ':' && (a + 1 == end || t[a + 1] == ' ')) {
            pt.key_s = p + 1;
            pt.key_e = q.inner_end;
            pt.flags |= line_flag::KEY_QUOTED;
            if (q.esc) pt.flags |= line_flag::KEY_ESC;
            val_from = a + 1;
        } else {
            bool has_comment = false;
            std::uint32_t stray = 0;
            if (!ends_in_comment_or_nothing(t, q.after, end, has_comment,
                                            stray))
                return false;
            pt.kind = seq ? LineKind::SeqItem : LineKind::Root;
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
            std::uint32_t ve = comment != NONE ? comment : end;
            while (ve > p && t[ve - 1] == ' ') --ve;
            pt.kind = seq ? LineKind::SeqItem : LineKind::Root;
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
    if (vc == '|' || vc == '>') return classify_block_header(t, v, end, pt);
    if (vc == '"' || vc == '\'') {
        Quoted q;
        if (!scan_quoted(t, v, end, q)) {
            open_quote(pt, v, end);
            return true;
        }
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

/// The scalar that goes on in the lines after its first.
enum class Open : std::uint8_t { None, Plain, Quoted, Block };

/// What the open scalar makes of a line.
enum class Step : std::uint8_t {
    /// The line belongs to the scalar.
    Consumed,
    /// The scalar ended before the line, which is read as any other.
    Pass,
    /// The text is outside the subset, or invalid.
    Fail,
};

// `---` or `...` at `i`, then a blank or the end of the line.
bool is_marker(const char* t, std::uint32_t i, std::uint32_t end) {
    return end - i >= 3 && (t[i] == '-' || t[i] == '.') && t[i + 1] == t[i] &&
           t[i + 2] == t[i] &&
           (i + 3 == end || t[i + 3] == ' ' || t[i + 3] == '\t');
}

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
    bool fail_escape(const detail::EscapeProblem& problem, std::uint32_t line);
    bool fail(std::uint32_t line, std::uint32_t offset, dstd::String reason);
    dstd::StringView key_view(std::uint32_t line, const LineParts& pt) const;
    void resolve_pending(bool child, std::uint32_t at);

    bool begin_scalar(std::uint32_t j, const LineParts& pt, std::int32_t limit,
                      bool is_strict);
    bool begin_next_line(std::uint32_t j, const LineParts& pt);
    bool deeper_line(std::uint32_t j, const LineParts& pt);
    Step continue_plain(std::uint32_t j, std::uint32_t start, std::uint32_t end,
                        std::uint8_t cr);
    Step open_line(std::uint32_t j, std::uint32_t start, std::uint32_t end,
                   std::uint8_t cr);
    Step plain_line(std::uint32_t j, std::uint32_t start, std::uint32_t end,
                    std::uint8_t cr);
    Step quoted_line(std::uint32_t j, std::uint32_t start, std::uint32_t end,
                     std::uint8_t cr);
    Step block_line(std::uint32_t j, std::uint32_t start, std::uint32_t end,
                    std::uint8_t cr);
    bool finish_plain();
    bool finish_block(std::uint32_t end_line);
    bool finish_open();
    void push_text(std::uint32_t owner, const detail::ScalarText& text,
                   std::uint32_t end_line);
    void complete_value();
    bool note_bad(std::uint32_t line, std::uint32_t offset, const char* reason);
    void set_cont(LineEntry& entry, std::uint8_t cr) {
        entry.indent = 0;
        entry.kind = LineKind::Cont;
        entry.flags = cr;
    }

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
    /// A value that ended: a deeper line is no part of it. `cont_strict_` is
    /// set for a value that a key holds, where the deeper line is an error.
    bool cont_active_ = false;
    bool cont_strict_ = false;
    /// The value that ended in `cont_active_` is a plain one that a deeper line
    /// may go on in; the scalar opens at that line.
    bool plain_pending_ = false;
    bool invalid_ = false;
    std::uint32_t cont_limit_ = 0;
    bool explicit_open_ = false;
    bool has_content_ = false;
    bool after_end_ = false;
    bool any_content_ = false;
    bool has_tab_ = false;

    Open open_ = Open::None;
    /// The line that holds the start of the open scalar, and the indent that
    /// its other lines must exceed (-1 for a top-level scalar).
    std::uint32_t owner_ = 0;
    /// The line that holds the first text of the scalar: the owner, or a later
    /// one when the value starts on its own lines.
    std::uint32_t first_line_ = 0;
    std::int32_t limit_ = -1;
    bool is_strict_ = false;
    /// A plain scalar: the text of its first line, the last line that goes on
    /// and where the text of that line ends.
    std::uint32_t first_s_ = 0;
    std::uint32_t first_e_ = 0;
    std::uint32_t last_ = 0;
    std::uint32_t last_e_ = 0;
    /// A quoted scalar: its opening quote, and the first thing wrong with a
    /// line of it, which counts once the scalar closes.
    std::uint32_t quote_at_ = 0;
    std::uint32_t bad_line_ = NONE;
    std::uint32_t bad_at_ = 0;
    const char* bad_reason_ = nullptr;
    /// A block scalar.
    detail::BlockSpec block_;
    bool is_explicit_indent_ = false;
    bool has_text_ = false;
    std::uint32_t max_blank_ = 0;
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
    if (!classify_line(t_, ix_.entries[line].start, ix_.line_end(line), pt))
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

bool Builder::fail_escape(const detail::EscapeProblem& problem,
                          std::uint32_t line) {
    switch (problem.kind) {
        case detail::EscapeProblem::Unknown:
            return fail(line, problem.at,
                        dstd::fmt<"unknown escape character '{}'">(
                            dstd::StringView(&problem.ch, 1)));
        case detail::EscapeProblem::Digits:
            return fail(line, problem.at,
                        dstd::fmt<"escape '\\{}' needs {} hex digits">(
                            dstd::StringView(&problem.ch, 1), problem.count));
        case detail::EscapeProblem::Hex:
            return fail(
                line, problem.at,
                dstd::fmt<"escape '\\{}' has a character that is not a hex "
                          "digit">(dstd::StringView(&problem.ch, 1)));
        default:
            return fail(line, problem.at, "escape is not a code point");
    }
}

bool Builder::decode_into(std::uint32_t line, bool is_value, std::uint32_t s,
                          std::uint32_t e) {
    const bool dq = t_[s - 1] == '"';
    detail::EscapeProblem problem;
    detail::ScalarText text;
    const detail::Decoded decoded =
        detail::decode_quoted(ix_, s, e, dq, problem, text);
    if (decoded == detail::Decoded::Decline) return false;
    if (decoded == detail::Decoded::Invalid) return fail_escape(problem, line);
    ix_.escapes.push_back(
        EscapeEntry{line * 2 + (is_value ? 1u : 0u), text.off, text.len, 0});
    return true;
}

void Builder::push_text(std::uint32_t owner, const detail::ScalarText& text,
                        std::uint32_t end_line) {
    const auto index = static_cast<std::uint32_t>(ix_.escapes.size());
    ix_.escapes.push_back(EscapeEntry{
        owner * 2 + 1, text.off, text.len,
        end_line | (text.in_text ? ESCAPE_IN_TEXT : std::uint32_t{0})});
    if (end_line > owner + 1) ix_.next[owner + 1] = index;
}

void Builder::complete_value() {
    if (limit_ < 0) return;
    cont_active_ = true;
    cont_limit_ = static_cast<std::uint32_t>(limit_);
    cont_strict_ = is_strict_;
}

bool Builder::note_bad(std::uint32_t line, std::uint32_t offset,
                       const char* reason) {
    if (bad_line_ == NONE) {
        bad_line_ = line;
        bad_at_ = offset;
        bad_reason_ = reason;
    }
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
    plain_pending_ = false;
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

// Opens the scalar that the first line `j` holds the start of, when its lines
// go on, or notes that its value is complete. The lines are scanned as they
// come and the text is decoded once the scalar ends, the split of rapidyaml
// between scanning a scalar and filtering it (MIT,
// https://github.com/biojppm/rapidyaml).
bool Builder::begin_scalar(std::uint32_t j, const LineParts& pt,
                           std::int32_t limit, bool is_strict) {
    owner_ = j;
    first_line_ = j;
    limit_ = limit;
    is_strict_ = is_strict;
    if (pt.open == LineOpen::Block) {
        const char* h = t_ + pt.open_at;
        block_ = detail::BlockSpec{};
        block_.is_literal = *h == '|';
        std::uint32_t digit = 0;
        for (const char* c = h + 1; c < t_ + pt.val_e; ++c) {
            if (*c == '-') {
                block_.chomp = -1;
            } else if (*c == '+') {
                block_.chomp = 1;
            } else {
                digit = static_cast<std::uint32_t>(*c - '0');
            }
        }
        is_explicit_indent_ = digit != 0;
        has_text_ = false;
        block_.is_indent_known = is_explicit_indent_;
        block_.indent =
            (limit > 0 ? static_cast<std::uint32_t>(limit) : 0u) + digit;
        max_blank_ = 0;
        open_ = Open::Block;
        return true;
    }
    if (pt.open == LineOpen::Quoted) {
        open_ = Open::Quoted;
        quote_at_ = pt.open_at;
        bad_line_ = NONE;
        return true;
    }
    if ((pt.flags & (line_flag::VAL_QUOTED | line_flag::FLOW_EMPTY |
                     line_flag::COMMENT)) == 0) {
        first_s_ = pt.val_s;
        first_e_ = pt.val_e;
        last_ = j;
        last_e_ = pt.val_e;
        open_ = Open::Plain;
        return true;
    }
    complete_value();
    return true;
}

// The value of the entry or item `pending_`, which has no text of its own,
// starts at the line `j`.
bool Builder::begin_next_line(std::uint32_t j, const LineParts& pt) {
    if (pt.open == LineOpen::Block ||
        ((pt.flags & line_flag::VAL_QUOTED) != 0 && pt.open == LineOpen::NONE))
        return false;
    owner_ = pending_;
    first_line_ = j;
    limit_ = static_cast<std::int32_t>(pending_col_);
    LineEntry& owner = ix_.entries[owner_];
    is_strict_ = owner.kind != LineKind::SeqItem;
    owner.flags = static_cast<std::uint8_t>(
        (owner.flags & ~line_flag::VAL_EMPTY) | line_flag::VAL_ESC);
    pending_ = NONE;
    LineEntry& entry = ix_.entries[j];
    set_cont(entry, entry.flags & line_flag::CR);
    if (pt.open == LineOpen::Quoted) {
        open_ = Open::Quoted;
        quote_at_ = pt.open_at;
        bad_line_ = NONE;
        return true;
    }
    open_ = Open::Plain;
    first_s_ = pt.val_s;
    first_e_ = pt.val_e;
    last_ = j;
    last_e_ = pt.val_e;
    if ((pt.flags & line_flag::COMMENT) != 0) return finish_plain();
    return true;
}

Step Builder::open_line(std::uint32_t j, std::uint32_t start, std::uint32_t end,
                        std::uint8_t cr) {
    switch (open_) {
        case Open::Plain:
            return plain_line(j, start, end, cr);
        case Open::Quoted:
            return quoted_line(j, start, end, cr);
        default:
            return block_line(j, start, end, cr);
    }
}

bool Builder::finish_plain() {
    open_ = Open::None;
    if (first_line_ != owner_ && last_ == first_line_ &&
        is_null_text(dstd::StringView(t_ + first_s_, first_e_ - first_s_)))
        return false;
    if (last_ != owner_) {
        for (std::uint32_t l = owner_ + 1; l <= last_; ++l)
            ix_.entries[l].kind = LineKind::Cont;
        detail::ScalarText text;
        detail::decode_plain_lines(ix_, first_line_, last_, first_s_, first_e_,
                                   last_e_, text);
        push_text(owner_, text, last_ + 1);
        ix_.entries[owner_].flags |= line_flag::VAL_ESC;
    }
    complete_value();
    return true;
}

Step Builder::plain_line(std::uint32_t j, std::uint32_t start,
                         std::uint32_t end, std::uint8_t cr) {
    std::uint32_t i = start;
    while (i < end && t_[i] == ' ') ++i;
    const std::uint32_t n = i - start;
    LineEntry& entry = ix_.entries[j];
    if (i == end) {
        if (n > INDENT_LIMIT) return Step::Fail;
        entry.indent = static_cast<std::uint16_t>(n);
        entry.kind = LineKind::Blank;
        entry.flags = cr;
        return Step::Consumed;
    }
    if ((limit_ >= 0 && n <= static_cast<std::uint32_t>(limit_)) ||
        (n == 0 && is_marker(t_, i, end)) || t_[i] == '#')
        return finish_plain() ? Step::Pass : Step::Fail;
    if (has_tab_ && std::memchr(t_ + i, '\t', end - i) != nullptr)
        return Step::Fail;
    std::uint32_t comment = NONE;
    for (std::uint32_t k = i; k < end; ++k) {
        const char ch = t_[k];
        if (ch == ':' && (k + 1 == end || t_[k + 1] == ' ')) {
            fail(j, i,
                 "mapping values are not allowed in a multi-line plain "
                 "scalar");
            return Step::Fail;
        }
        if (ch == ' ' && k + 1 < end && t_[k + 1] == '#') {
            comment = k;
            break;
        }
    }
    std::uint32_t te = comment != NONE ? comment : end;
    while (te > i && t_[te - 1] == ' ') --te;
    set_cont(entry, cr);
    last_ = j;
    last_e_ = te;
    if (comment != NONE) return finish_plain() ? Step::Consumed : Step::Fail;
    return Step::Consumed;
}

Step Builder::quoted_line(std::uint32_t j, std::uint32_t start,
                          std::uint32_t end, std::uint8_t cr) {
    if (has_tab_ && std::memchr(t_ + start, '\t', end - start) != nullptr)
        return Step::Fail;
    std::uint32_t i = start;
    while (i < end && t_[i] == ' ') ++i;
    const std::uint32_t n = i - start;
    set_cont(ix_.entries[j], cr);
    if (i == end) return Step::Consumed;
    if (limit_ >= 0 && n <= static_cast<std::uint32_t>(limit_)) {
        note_bad(j, i, "a quoted scalar is not indented enough");
    } else if (n == 0 && is_marker(t_, i, end)) {
        note_bad(j, i, "a document marker inside a quoted scalar");
    }
    const std::uint32_t close = find_close(t_, i, end, t_[quote_at_]);
    if (close == NONE) return Step::Consumed;
    if (bad_line_ != NONE) {
        fail(bad_line_, bad_at_, bad_reason_);
        return Step::Fail;
    }
    bool has_comment = false;
    std::uint32_t stray = 0;
    if (!ends_in_comment_or_nothing(t_, close + 1, end, has_comment, stray)) {
        if (is_strict_ && dstd::is_ascii_alnum(t_[stray])) {
            fail(j, stray, "unexpected text after a quoted scalar");
        }
        return Step::Fail;
    }
    detail::EscapeProblem problem;
    detail::ScalarText text;
    for (std::uint32_t l = owner_ + 1; l < first_line_; ++l)
        ix_.entries[l].kind = LineKind::Cont;
    const detail::Decoded decoded = detail::decode_quoted_lines(
        ix_, first_line_, j, quote_at_, close, problem, text);
    if (decoded == detail::Decoded::Decline) return Step::Fail;
    if (decoded == detail::Decoded::Invalid) {
        fail_escape(problem, problem.line);
        return Step::Fail;
    }
    open_ = Open::None;
    push_text(owner_, text, j + 1);
    complete_value();
    return Step::Consumed;
}

bool Builder::finish_block(std::uint32_t end_line) {
    open_ = Open::None;
    if (is_explicit_indent_ && end_line == owner_ + 1 &&
        end_line != line_count_)
        return false;
    if (!has_text_ && block_.is_indent_known) {
        for (std::uint32_t l = owner_ + 1; l < end_line; ++l) {
            std::uint32_t b = ix_.entries[l].start;
            const std::uint32_t e = ix_.line_end(l);
            while (b < e && t_[b] == ' ') ++b;
            if (b - ix_.entries[l].start > block_.indent) return false;
        }
    }
    detail::ScalarText text;
    if (detail::decode_block(ix_, block_, owner_ + 1, end_line, text) !=
        detail::Decoded::Ok)
        return false;
    push_text(owner_, text, end_line);
    complete_value();
    return true;
}

Step Builder::block_line(std::uint32_t j, std::uint32_t start,
                         std::uint32_t end, std::uint8_t cr) {
    std::uint32_t i = start;
    while (i < end && t_[i] == ' ') ++i;
    const std::uint32_t spaces = i - start;
    if (i != end) {
        if (spaces == 0 && is_marker(t_, i, end))
            return finish_block(j) ? Step::Pass : Step::Fail;
        if (t_[i] == '\t') {
            bool is_white = true;
            for (std::uint32_t k = i; k < end && is_white; ++k)
                is_white = t_[k] == ' ' || t_[k] == '\t';
            if (is_white) return Step::Fail;
        }
        if (!block_.is_indent_known) {
            if (static_cast<std::int64_t>(spaces) <= limit_)
                return finish_block(j) ? Step::Pass : Step::Fail;
            if (t_[i] == '\t') return Step::Fail;
            block_.indent = spaces;
            block_.is_indent_known = true;
            if (max_blank_ > spaces) {
                for (std::uint32_t l = owner_ + 1; l < j; ++l) {
                    std::uint32_t b = ix_.entries[l].start;
                    const std::uint32_t e = ix_.line_end(l);
                    while (b < e && t_[b] == ' ') ++b;
                    if (b - ix_.entries[l].start > spaces) {
                        fail(l, ix_.entries[l].start + spaces,
                             "an empty line of a block scalar is more "
                             "indented than its first line of text");
                        return Step::Fail;
                    }
                }
            }
        } else if (spaces < block_.indent) {
            if (is_explicit_indent_ && !has_text_ && t_[i] != '#' &&
                static_cast<std::int64_t>(spaces) > limit_) {
                if (!is_strict_) return Step::Fail;
                fail(j, start + spaces,
                     "a block scalar is less indented than its indentation "
                     "indicator");
                return Step::Fail;
            }
            return finish_block(j) ? Step::Pass : Step::Fail;
        }
        has_text_ = true;
    } else if (!block_.is_indent_known) {
        max_blank_ = std::max(max_blank_, spaces);
    }
    set_cont(ix_.entries[j], cr);
    return Step::Consumed;
}

bool Builder::finish_open() {
    switch (open_) {
        case Open::Plain:
            return finish_plain();
        case Open::Block:
            return finish_block(line_count_);
        case Open::Quoted:
            return fail(owner_, quote_at_, "unterminated quoted scalar");
        default:
            return true;
    }
}

// A line deeper than the value that ended before it goes on in that value,
// which the line index declines, unless a key holds the value: then it is an
// error.
bool Builder::deeper_line(std::uint32_t j, const LineParts& pt) {
    if (after_end_) return false;
    has_content_ = true;
    any_content_ = true;
    if (cont_strict_)
        return fail(j, ix_.entries[j].start + pt.indent,
                    "a complete value cannot go on in a deeper line");
    return false;
}

// The plain value of the line `owner_` goes on in the line `j`, or may do so
// after it: the scalar opens at the owner, which is read again.
Step Builder::continue_plain(std::uint32_t j, std::uint32_t start,
                             std::uint32_t end, std::uint8_t cr) {
    plain_pending_ = false;
    LineParts first;
    classify_line(t_, ix_.entries[owner_].start, ix_.line_end(owner_), first);
    first_line_ = owner_;
    limit_ = static_cast<std::int32_t>(cont_limit_);
    is_strict_ = cont_strict_;
    first_s_ = first.val_s;
    first_e_ = first.val_e;
    last_ = owner_;
    last_e_ = first.val_e;
    open_ = Open::Plain;
    return plain_line(j, start, end, cr);
}

bool Builder::structure(std::uint32_t j, const LineParts& pt) {
    if (after_end_) return false;
    const std::uint32_t n = pt.indent;
    const bool is_root =
        pt.kind == LineKind::Root || pt.kind == LineKind::RootDoc;
    const bool is_root_allowed = depth_ == 0 && !has_content_ && n == 0;
    if (is_root && pending_ != NONE && n > pending_col_)
        return begin_next_line(j, pt);
    has_content_ = true;
    any_content_ = true;
    cont_active_ = false;
    plain_pending_ = false;
    if (is_root) {
        if (!is_root_allowed) return false;
        after_end_ = true;
        return begin_scalar(j, pt, -1, false);
    }
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
        } else if (((pt.flags & (line_flag::VAL_QUOTED | line_flag::FLOW_EMPTY |
                                 line_flag::COMMENT)) |
                    pt.open) == 0) {
            // A plain value: a deeper line may go on in it, and the scalar
            // opens there.
            owner_ = j;
            cont_active_ = true;
            cont_limit_ = limit;
            cont_strict_ = pt.kind != LineKind::SeqItem;
            plain_pending_ = true;
        } else {
            return begin_scalar(j, pt, static_cast<std::int32_t>(limit),
                                pt.kind != LineKind::SeqItem);
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
    if ((static_cast<unsigned>(open_) | static_cast<unsigned>(has_tab_)) != 0)
        [[unlikely]] {
        if (open_ != Open::None) {
            const Step step = open_line(j, start, end, flags);
            if (step == Step::Consumed) return true;
            if (step == Step::Fail) return false;
        }
        if (has_tab_ && std::memchr(t_ + start, '\t', end - start) != nullptr)
            return false;
    }
    LineParts pt;
    const bool plain = batch_ != nullptr && start >= batch_->base &&
                       classify_plain(*batch_, t_, start, end, pt);
    if (!plain && !classify_line(t_, start, end, pt)) {
        if (plain_pending_ && pt.indent > cont_limit_) {
            const Step step = continue_plain(j, start, end, flags);
            if (step == Step::Consumed) return true;
            if (step == Step::Fail) return false;
        }
        // A line that goes on a value that ended may hold any text.
        if (cont_active_ && pt.indent > cont_limit_) {
            if (cont_strict_ && pt.error == LineError::NONE &&
                pt.indent <= INDENT_LIMIT && start + pt.indent != end)
                return fail(j, start + pt.indent,
                            "a complete value cannot go on in a deeper line");
            return false;
        }
        if (after_end_) return false;
        switch (pt.error) {
            case LineError::TrailingText:
                return fail(j, pt.error_at,
                            "unexpected text after a quoted scalar");
            case LineError::MapValue:
                return fail(j, pt.error_at,
                            "mapping values are not allowed in this context");
            case LineError::BlockHeader:
                return fail(j, pt.error_at,
                            "a block scalar header holds text that is not an "
                            "indicator or a comment");
            default:
                return false;
        }
    }
    entry.indent = static_cast<std::uint16_t>(pt.indent);
    entry.kind = pt.kind;
    entry.flags = static_cast<std::uint8_t>(flags | pt.flags);
    switch (pt.kind) {
        case LineKind::Blank:
            if (plain_pending_) {
                const Step step = continue_plain(j, start, end, flags);
                return step == Step::Consumed;
            }
            return true;
        case LineKind::Comment:
            plain_pending_ = false;
            return true;
        case LineKind::DocStart:
        case LineKind::DocEnd:
            return marker(j, pt.kind);
        case LineKind::RootDoc:
            if (!marker(j, LineKind::DocStart)) return false;
            break;
        default:
            break;
    }
    if (cont_active_ && pt.indent > cont_limit_) {
        if (plain_pending_) {
            const Step step = continue_plain(j, start, end, flags);
            if (step == Step::Consumed) return true;
            if (step == Step::Fail) return false;
        }
        // A line that goes on a value that ended is text, not a scalar to
        // decode.
        return deeper_line(j, pt);
    }
    if ((pt.flags & (line_flag::KEY_ESC | line_flag::VAL_ESC)) != 0) {
        if ((pt.flags & line_flag::KEY_ESC) != 0 &&
            !decode_into(j, false, pt.key_s, pt.key_e))
            return false;
        if ((pt.flags & line_flag::VAL_ESC) != 0 && pt.open == LineOpen::NONE &&
            !decode_into(j, true, pt.val_s, pt.val_e))
            return false;
    }
    return structure(j, pt);
}

bool Builder::build(std::size_t size) {
    const auto text_size = static_cast<std::uint32_t>(size);
    const bool ends_in_newline = t_[size - 1] == '\n';
    line_count_ = static_cast<std::uint32_t>(detail::count_newlines(t_, size) +
                                             (ends_in_newline ? 0 : 1));
    if (size >= SHORT_LINE_CHECK_SIZE && line_count_ > size / MIN_AVERAGE_LINE)
        return false;
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
            // The positions of the set bits: 8 written with no branch, then
            // the rest, as the bit flattening of simdjson (Apache-2.0 OR MIT,
            // https://github.com/simdjson/simdjson; Langdale and Lemire,
            // https://arxiv.org/abs/1902.08318).
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
        if (any_bad != 0) {
            // A TAB is no fault of a block scalar, so the lines decide.
            for (std::size_t b = 0; b < blocks; ++b) {
                std::uint64_t m = bad[b];
                while (m != 0) {
                    const std::size_t at =
                        off + b * 64 +
                        static_cast<std::size_t>(dstd::bits::ctz_u64(m));
                    if (t_[at] != '\t') return false;
                    has_tab_ = true;
                    m &= m - 1;
                }
            }
        }
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
    if (!finish_open()) return false;
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
    arena.release();
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
