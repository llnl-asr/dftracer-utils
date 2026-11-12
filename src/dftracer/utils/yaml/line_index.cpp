// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/export.h>
#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/bits.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/yaml/flow_parser.h>
#include <dftracer/utils/yaml/key_set.h>
#include <dftracer/utils/yaml/line_index.h>
#include <dftracer/utils/yaml/line_scalar.h>
#include <dftracer/utils/yaml/scalar.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>

namespace dftracer::utils::yaml {

namespace {

constexpr std::uint32_t NONE = 0xFFFFFFFFu;
constexpr std::size_t BATCH_BLOCKS = 64;
constexpr std::uint64_t TEXT_SIZE_LIMIT = 0xFFFFFFF0ull;
constexpr std::uint32_t INDENT_LIMIT = 0xFFFE;
// classify_plain reads a line shorter than this from one short_window.
constexpr std::uint32_t SHORT_WINDOW_BITS = 57;

bool is_white(char c) { return c == ' ' || c == '\t'; }

bool refuse(LineParts& pt, std::uint32_t at, const char* reason) {
    pt.reason = reason;
    pt.error_at = at;
    return false;
}

// A `:` at `k` that separates a key from its value: a blank or the end of the
// line follows it.
bool is_value_sep(const char* t, std::uint32_t k, std::uint32_t end) {
    return t[k] == ':' && (k + 1 == end || is_white(t[k + 1]));
}

// A `-` at `k` that makes a sequence item: a blank or the end of the line
// follows it.
bool is_item_dash(const char* t, std::uint32_t k, std::uint32_t end) {
    return t[k] == '-' && (k + 1 == end || is_white(t[k + 1]));
}

// Whether the byte at `p` can start a plain scalar, a key or a value that has
// no other start. A feature that is not supported, and a byte that YAML
// reserves, are refused with a reason. `is_key` says that a key can start
// here, so a `:` followed by a blank is a key with no text.
bool check_lead(const char* t, std::uint32_t p, std::uint32_t end, bool is_key,
                LineParts& pt) {
    switch (t[p]) {
        case '&':
            return refuse(pt, p, "anchors are not supported");
        case '*':
            return refuse(pt, p, "aliases are not supported");
        case '!':
            return refuse(pt, p, "tags are not supported");
        case '%':
            return refuse(pt, p, "a plain scalar cannot start with '%'");
        case '@':
            return refuse(pt, p,
                          "'@' is reserved and cannot start a plain scalar");
        case '`':
            return refuse(pt, p,
                          "'`' is reserved and cannot start a plain scalar");
        case ',':
        case ']':
        case '}':
            return refuse(pt, p,
                          "a plain scalar cannot start with a flow indicator");
        case '?':
            if (p + 1 == end || is_white(t[p + 1]))
                return refuse(pt, p, "complex keys are not supported");
            return true;
        case ':':
            if (p + 1 == end || is_white(t[p + 1]))
                return refuse(pt, p,
                              is_key ? "a null key is not supported"
                                     : "mapping values are not allowed in this "
                                       "context");
            return true;
        default:
            return true;
    }
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

// What follows a scalar: nothing, or a comment set off by a blank.
bool ends_in_comment_or_nothing(const char* t, std::uint32_t after,
                                std::uint32_t end, bool& has_comment,
                                std::uint32_t& stray) {
    std::uint32_t a = after;
    while (a < end && is_white(t[a])) ++a;
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

// A flow collection that starts at `at`: the flow parser reads it, and it may
// go on in later lines.
void open_flow(LineParts& pt, std::uint32_t at) {
    pt.open = LineOpen::Flow;
    pt.open_at = at;
    pt.val_s = at;
}

// The header of the block scalar whose `|` or `>` is at `p`: an indentation
// indicator and a chomping indicator in either order, then nothing or a
// comment set off by a blank (YAML 1.2.2, 8.1.1, https://yaml.org/spec/1.2.2/).
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
    while (a < end && is_white(t[a])) ++a;
    if (a < end && !(t[a] == '#' && a > i))
        return refuse(pt, a,
                      "a block scalar header holds text that is not an "
                      "indicator or a comment");
    pt.open = LineOpen::Block;
    pt.open_at = p;
    pt.val_s = p;
    pt.val_e = i;
    pt.flags |= line_flag::VAL_ESC | line_flag::VAL_QUOTED;
    if (a < end) pt.flags |= line_flag::COMMENT;
    return true;
}

// `~`, `null`, `Null` and `NULL`, which read as a null key.
bool is_null_key(dstd::StringView key) {
    const char* k = key.data();
    switch (key.size()) {
        case 1:
            return k[0] == '~';
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

// The scan of a plain scalar that starts at `from`: where a `: ` that makes a
// key is, and where a comment starts.
struct PlainScan {
    std::uint32_t sep = NONE;
    std::uint32_t comment = NONE;
};

PlainScan scan_plain(const char* t, std::uint32_t from, std::uint32_t end) {
    PlainScan scan;
    for (std::uint32_t k = from; k < end; ++k) {
        const char ch = t[k];
        if (ch == ':' && (k + 1 == end || is_white(t[k + 1]))) {
            scan.sep = k;
            break;
        }
        if (is_white(ch) && k + 1 < end && t[k + 1] == '#') {
            scan.comment = k;
            break;
        }
    }
    return scan;
}

// The top-level node that follows `---` on the line, which starts at `k`.
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
            return refuse(pt, stray, "unexpected text after a quoted scalar");
        pt.val_s = k + 1;
        pt.val_e = q.inner_end;
        pt.flags |= line_flag::VAL_QUOTED;
        if (q.esc) pt.flags |= line_flag::VAL_ESC;
        if (has_comment) pt.flags |= line_flag::COMMENT;
        return true;
    }
    if (c == '[' || c == '{') {
        open_flow(pt, k);
        return true;
    }
    if (!check_lead(t, k, end, false, pt)) return false;
    if (is_item_dash(t, k, end))
        return refuse(pt, k,
                      "a sequence entry cannot start on the line of a "
                      "document marker");
    const PlainScan scan = scan_plain(t, k, end);
    if (scan.sep != NONE)
        return refuse(pt, scan.sep,
                      "mapping values are not allowed in this context");
    std::uint32_t ve = scan.comment != NONE ? scan.comment : end;
    while (ve > k && is_white(t[ve - 1])) --ve;
    pt.val_s = k;
    pt.val_e = ve;
    if (scan.comment != NONE) pt.flags |= line_flag::COMMENT;
    return true;
}

// The classifier of a line, which classify_line wraps. A TAB that follows the
// spaces of the line is white space, and where it is a lead the offset is in
// `pt.tab_lead`.
bool classify_content(const char* t, std::uint32_t start, std::uint32_t end,
                      LineParts& pt) {
    pt = LineParts{};
    std::uint32_t i = start;
    while (i < end && t[i] == ' ') ++i;
    pt.indent = i - start;
    if (pt.indent > INDENT_LIMIT)
        return refuse(pt, i,
                      "indentation deeper than 65534 columns is not "
                      "supported");
    if (i == end) {
        pt.kind = LineKind::Blank;
        return true;
    }
    if (t[i] == '\t') {
        std::uint32_t k = i;
        while (k < end && is_white(t[k])) ++k;
        if (k == end) {
            pt.kind = LineKind::Blank;
            return true;
        }
        if (t[k] == '#') {
            pt.kind = LineKind::Comment;
            return true;
        }
        pt.has_tab_lead = true;
        pt.error_at = i;
        i = k;
    }
    if (t[i] == '#') {
        pt.kind = LineKind::Comment;
        return true;
    }
    if (pt.indent == 0 && !pt.has_tab_lead && (t[i] == '-' || t[i] == '.') &&
        end - i >= 3 && t[i + 1] == t[i] && t[i + 2] == t[i] &&
        (i + 3 == end || is_white(t[i + 3]))) {
        std::uint32_t k = i + 3;
        while (k < end && is_white(t[k])) ++k;
        if (k < end && t[k] != '#') {
            if (t[i] != '-')
                return refuse(pt, k,
                              "unexpected text after a document end marker");
            return classify_root_doc(t, k, end, pt);
        }
        pt.kind = t[i] == '-' ? LineKind::DocStart : LineKind::DocEnd;
        return true;
    }
    if (pt.indent == 0 && !pt.has_tab_lead && t[i] == '%')
        return refuse(pt, i, "directives are not supported");
    bool seq = false;
    std::uint32_t p = i;
    std::uint32_t dash_tab = NONE;
    if (is_item_dash(t, i, end)) {
        seq = true;
        pt.dash = i;
        p = i + 1;
        const auto skip_white = [&] {
            dash_tab = NONE;
            while (p < end && is_white(t[p])) {
                if (t[p] == '\t' && dash_tab == NONE) dash_tab = p;
                ++p;
            }
        };
        skip_white();
        while (p < end && is_item_dash(t, p, end)) {
            if (dash_tab != NONE)
                return refuse(pt, dash_tab,
                              "a tab cannot separate a block indicator");
            ++pt.nest;
            ++p;
            skip_white();
        }
        pt.kind = pt.nest != 0 ? LineKind::NestItem : LineKind::SeqItem;
        pt.content = p;
        if (p == end) {
            pt.flags = line_flag::VAL_EMPTY;
            return true;
        }
        if (t[p] == '#') {
            pt.flags = line_flag::VAL_EMPTY | line_flag::COMMENT;
            return true;
        }
    }
    pt.content = p;
    const char c = t[p];
    const LineKind item_kind =
        seq ? (pt.nest != 0 ? LineKind::NestItem : LineKind::SeqItem)
            : LineKind::Root;
    const LineKind map_kind =
        seq ? (pt.nest != 0 ? LineKind::NestMap : LineKind::SeqMap)
            : LineKind::MapEntry;
    std::uint32_t val_from = 0;
    if (c == '|' || c == '>') {
        pt.kind = item_kind;
        return classify_block_header(t, p, end, pt);
    }
    if (c == '"' || c == '\'') {
        Quoted q;
        if (!scan_quoted(t, p, end, q)) {
            pt.kind = item_kind;
            open_quote(pt, p, end);
            return true;
        }
        std::uint32_t a = q.after;
        while (a < end && is_white(t[a])) ++a;
        if (a < end && is_value_sep(t, a, end)) {
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
                return refuse(pt, stray,
                              "unexpected text after a quoted scalar");
            pt.kind = item_kind;
            pt.val_s = p + 1;
            pt.val_e = q.inner_end;
            pt.flags |= line_flag::VAL_QUOTED;
            if (q.esc) pt.flags |= line_flag::VAL_ESC;
            if (has_comment) pt.flags |= line_flag::COMMENT;
            return true;
        }
    } else {
        if (!check_lead(t, p, end, true, pt)) return false;
        if (c == '[' || c == '{') {
            pt.kind = item_kind;
            open_flow(pt, p);
            return true;
        }
        const PlainScan scan = scan_plain(t, p, end);
        if (scan.sep == NONE) {
            std::uint32_t ve = scan.comment != NONE ? scan.comment : end;
            while (ve > p && is_white(t[ve - 1])) --ve;
            pt.kind = item_kind;
            pt.val_s = p;
            pt.val_e = ve;
            if (scan.comment != NONE) pt.flags |= line_flag::COMMENT;
            return true;
        }
        std::uint32_t ke = scan.sep;
        while (ke > p && is_white(t[ke - 1])) --ke;
        if (is_null_key(dstd::StringView(t + p, ke - p)))
            return refuse(pt, p, "a null key is not supported");
        pt.key_s = p;
        pt.key_e = ke;
        val_from = scan.sep + 1;
    }
    if (dash_tab != NONE)
        return refuse(pt, dash_tab, "a tab cannot separate a block indicator");
    pt.kind = map_kind;
    std::uint32_t v = val_from;
    while (v < end && is_white(t[v])) ++v;
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
        if (!ends_in_comment_or_nothing(t, q.after, end, has_comment, stray))
            return refuse(pt, stray, "unexpected text after a quoted scalar");
        pt.val_s = v + 1;
        pt.val_e = q.inner_end;
        pt.flags |= line_flag::VAL_QUOTED;
        if (q.esc) pt.flags |= line_flag::VAL_ESC;
        if (has_comment) pt.flags |= line_flag::COMMENT;
        return true;
    }
    if (vc == '[' || vc == '{') {
        open_flow(pt, v);
        return true;
    }
    if (!check_lead(t, v, end, false, pt)) return false;
    if (is_item_dash(t, v, end))
        return refuse(pt, v, "a sequence entry is not allowed in this context");
    const PlainScan scan = scan_plain(t, v, end);
    if (scan.sep != NONE)
        return refuse(pt, scan.sep,
                      "mapping values are not allowed in this context");
    std::uint32_t ve = scan.comment != NONE ? scan.comment : end;
    while (ve > v && is_white(t[ve - 1])) --ve;
    pt.val_s = v;
    pt.val_e = ve;
    if (scan.comment != NONE) pt.flags |= line_flag::COMMENT;
    return true;
}

}  // namespace

bool classify_line(const char* t, std::uint32_t start, std::uint32_t end,
                   LineParts& pt) {
    if (!classify_content(t, start, end, pt)) return false;
    // A TAB between the spaces of a line and its content is white space, so
    // only a scalar or a flow collection may start after it.
    if (pt.has_tab_lead && pt.kind != LineKind::Root)
        return refuse(pt, pt.error_at, "a tab cannot be used as indentation");
    return true;
}

namespace {

// The sequences of 0x80 and above that a YAML reader refuses: a UTF-8 text that
// is not well formed, and the code points that YAML 1.2.2 does not print (the
// C1 controls other than NEL, and U+FFFE and U+FFFF). `pending` is the count of
// continuation bytes that the last call left open, for a sequence that crosses
// the end of a block. Returns the offset of the first fault, or NONE.
std::uint32_t find_bad_utf8(const char* t, std::uint32_t from, std::uint32_t to,
                            std::uint32_t size, std::uint32_t& pending) {
    std::uint32_t i = from;
    while (pending != 0 && i < to) {
        if ((static_cast<unsigned char>(t[i]) & 0xC0) != 0x80) return i;
        --pending;
        ++i;
    }
    for (; i < to; ++i) {
        const auto c = static_cast<unsigned char>(t[i]);
        if (c < 0x80) continue;
        const auto n1 = i + 1 < size ? static_cast<unsigned char>(t[i + 1]) : 0;
        const auto n2 = i + 2 < size ? static_cast<unsigned char>(t[i + 2]) : 0;
        const auto n3 = i + 3 < size ? static_cast<unsigned char>(t[i + 3]) : 0;
        std::uint32_t len = 0;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            if ((n1 & 0xC0) != 0x80) return i;
            if (c == 0xC2 &&
                ((n1 >= 0x80 && n1 <= 0x84) || (n1 >= 0x86 && n1 <= 0x9F)))
                return i;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            if ((n1 & 0xC0) != 0x80 || (n2 & 0xC0) != 0x80) return i;
            if (c == 0xE0 && n1 < 0xA0) return i;
            if (c == 0xED && n1 > 0x9F) return i;
            if (c == 0xEF && n1 == 0xBF && (n2 == 0xBE || n2 == 0xBF)) return i;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            if ((n1 & 0xC0) != 0x80 || (n2 & 0xC0) != 0x80 ||
                (n3 & 0xC0) != 0x80)
                return i;
            if (c == 0xF0 && n1 < 0x90) return i;
            if (c == 0xF4 && n1 > 0x8F) return i;
        } else {
            return i;
        }
        // The bytes of the sequence past the end of this range are checked
        // above, so only the count that lies beyond `to` is left open.
        if (i + len > to) {
            pending = i + len - to;
            return NONE;
        }
        i += len - 1;
    }
    return NONE;
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

// At least the 57 bits of `a` that start at bit `pos`, from one load where
// the bytes of a word are in bit order. The array has a zero word after its
// last block, so the load stays inside it.
inline std::uint64_t short_window(const std::uint64_t* a, std::uint32_t pos) {
    if constexpr (std::endian::native == std::endian::little) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, reinterpret_cast<const char*>(a) + (pos >> 3), 8);
        return bits >> (pos & 7);
    } else {
        return window(a, pos);
    }
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

// classify_plain for a line of SHORT_WINDOW_BITS bytes or more.
DFTRACER_UTILS_NOINLINE bool classify_plain_long(const Batch& b, const char* t,
                                                 std::uint32_t start,
                                                 std::uint32_t end,
                                                 LineParts& pt) {
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

// One past the highest set bit of `x`, which is not 0.
inline std::uint32_t top_end(std::uint64_t x) {
    return 64u - static_cast<std::uint32_t>(dstd::bits::clz_u64(x));
}

inline std::uint32_t low_bit(std::uint64_t x) {
    return static_cast<std::uint32_t>(dstd::bits::ctz_u64(x));
}

// The facts of a plain line that the structure of the index needs.
struct PlainLine {
    std::uint32_t indent;
    std::uint32_t content;
    std::uint32_t key_s;
    std::uint32_t key_e;
    LineKind kind;
    std::uint8_t flags;
};

// The line `[start, end)` when it holds none of the bytes that set a line
// apart from a plain one: the same facts as classify_line, read from the bit
// arrays. False means the line goes to classify_line, whatever it holds. A
// line shorter than SHORT_WINDOW_BITS is one window of each array, so each
// search is one bit operation.
DFTRACER_UTILS_ALWAYS_INLINE inline bool classify_plain(const Batch& b,
                                                        const char* t,
                                                        std::uint32_t start,
                                                        std::uint32_t end,
                                                        PlainLine& pl) {
    const std::uint32_t rs = start - b.base;
    const std::uint32_t len = end - start;
    if (len >= SHORT_WINDOW_BITS) [[unlikely]] {
        LineParts pt;
        if (!classify_plain_long(b, t, start, end, pt)) return false;
        pl.indent = pt.indent;
        pl.content = pt.content;
        pl.key_s = pt.key_s;
        pl.key_e = pt.key_e;
        pl.kind = pt.kind;
        pl.flags = pt.flags;
        return true;
    }
    const std::uint64_t in_line = (std::uint64_t{1} << len) - 1;
    if ((short_window(b.special, rs) & in_line) != 0) return false;
    const std::uint64_t solid = ~short_window(b.space, rs) & in_line;
    pl.flags = 0;
    if (solid == 0) {
        pl.indent = len;
        pl.kind = LineKind::Blank;
        return true;
    }
    const std::uint32_t ri = low_bit(solid);
    const std::uint32_t i = start + ri;
    pl.indent = ri;
    const char c = t[i];
    if (c == ':') return false;
    if (ri == 0 && (c == '-' || c == '.') && len >= 3 && t[i + 1] == c &&
        t[i + 2] == c)
        return false;
    bool seq = false;
    std::uint32_t rp = ri;
    if (c == '-' && (ri + 1 == len || t[i + 1] == ' ')) {
        seq = true;
        pl.kind = LineKind::SeqItem;
        const std::uint64_t after = solid & ~((std::uint64_t{2} << ri) - 1);
        if (after == 0) {
            pl.content = end;
            pl.flags = line_flag::VAL_EMPTY;
            return true;
        }
        rp = low_bit(after);
        const std::uint32_t p = start + rp;
        if (t[p] == ':') return false;
        if (t[p] == '-' && (rp + 1 == len || t[p + 1] == ' ')) return false;
    }
    const std::uint32_t p = start + rp;
    pl.content = p;
    const std::uint64_t seps =
        short_window(b.sep, rs) & in_line & ~((std::uint64_t{1} << rp) - 1);
    if (seps == 0) return seq;
    const std::uint32_t rsep = low_bit(seps);
    // t[p] is no ':', so the key holds a byte that is no space.
    const std::uint32_t ke =
        start + top_end(solid & ((std::uint64_t{1} << rsep) - 1));
    if (ke - p <= 4 && is_null_key(dstd::StringView(t + p, ke - p)))
        return false;
    pl.key_s = p;
    pl.key_e = ke;
    pl.kind = seq ? LineKind::SeqMap : LineKind::MapEntry;
    const std::uint64_t value = solid & ~((std::uint64_t{2} << rsep) - 1);
    if (value == 0) {
        pl.flags = line_flag::VAL_EMPTY;
        return true;
    }
    const std::uint32_t rv = low_bit(value);
    const std::uint32_t v = start + rv;
    if (t[v] == ':' || (t[v] == '-' && (rv + 1 == len || t[v + 1] == ' ')))
        return false;
    return (seps & ~((std::uint64_t{1} << rv) - 1)) == 0;
}

std::atomic<bool> g_slow_lines{false};

// The keys of a level are in a separate array, so that the fields that each
// line reads stay small.
struct Level {
    std::uint32_t cur;
    std::uint16_t col;
    bool is_seq;
    /// A sequence that sits at the column of the map that holds its key.
    bool inline_seq;
};

/// The scalar or flow collection that goes on in the lines after its first.
enum class Open : std::uint8_t { None, Plain, Quoted, Block, Flow };

/// What the open scalar makes of a line.
enum class Step : std::uint8_t {
    /// The line belongs to the scalar.
    Consumed,
    /// The scalar ended before the line, which is read as any other.
    Pass,
    /// The text is invalid: `problem` says where and why.
    Fail,
};

// `---` or `...` at `i`, then a blank or the end of the line.
bool is_marker(const char* t, std::uint32_t i, std::uint32_t end) {
    return end - i >= 3 && (t[i] == '-' || t[i] == '.') && t[i + 1] == t[i] &&
           t[i + 2] == t[i] && (i + 3 == end || is_white(t[i + 3]));
}

bool is_seq_kind(LineKind kind) {
    return kind == LineKind::SeqItem || kind == LineKind::SeqMap ||
           kind == LineKind::NestItem || kind == LineKind::NestMap;
}

class Builder {
   public:
    explicit Builder(LineIndex& ix) : ix_(ix), t_(ix.text), flow_(ix) {}

    bool build(std::size_t size);

   private:
    bool process_line(std::uint32_t start, std::uint32_t nl_pos);
    DFTRACER_UTILS_NOINLINE bool process_any_line(std::uint32_t start,
                                                  std::uint32_t nl_pos);
    bool structure_plain(std::uint32_t j, std::uint32_t line_start,
                         const PlainLine& pl);
    bool plain_key_value(std::uint32_t j, std::uint32_t line_start,
                         const PlainLine& pl, std::uint32_t n);
    DFTRACER_UTILS_NOINLINE bool duplicate_key(std::uint32_t j,
                                               std::uint32_t at,
                                               dstd::StringView key);
    const Batch* batch_ = nullptr;
    bool structure(std::uint32_t j, const LineParts& pt);
    DFTRACER_UTILS_NOINLINE bool marker(std::uint32_t j);
    bool push_level(std::uint32_t col, std::uint32_t j, bool is_seq);
    void pop_level(std::uint32_t at);
    bool note_key(std::uint32_t line, dstd::StringView key);
    DFTRACER_UTILS_NOINLINE bool same_key(std::uint32_t line,
                                          dstd::StringView key) const;
    bool decode_into(std::uint32_t line, bool is_value, std::uint32_t s,
                     std::uint32_t e);
    bool fail_escape(const detail::EscapeProblem& problem);
    DFTRACER_UTILS_NOINLINE bool fail(std::uint32_t line, std::uint32_t offset,
                                      dstd::String reason);
    DFTRACER_UTILS_NOINLINE bool fail(std::uint32_t line, std::uint32_t offset,
                                      const char* reason);
    DFTRACER_UTILS_NOINLINE bool fail_at(std::uint32_t offset,
                                         dstd::String reason);
    DFTRACER_UTILS_NOINLINE bool fail_at(std::uint32_t offset,
                                         const char* reason);
    dstd::StringView key_view(std::uint32_t line, const LineParts& pt) const;
    void resolve_pending(bool child);

    bool begin_scalar(std::uint32_t j, const LineParts& pt, std::int32_t limit,
                      bool is_strict);
    DFTRACER_UTILS_NOINLINE bool begin_next_line(std::uint32_t j,
                                                 const LineParts& pt);
    DFTRACER_UTILS_NOINLINE void begin_block(const LineParts& pt,
                                             std::uint32_t first_line);
    bool begin_flow(std::uint32_t owner, std::uint32_t j, const LineParts& pt);
    DFTRACER_UTILS_NOINLINE bool deeper_line(std::uint32_t j,
                                             const LineParts& pt);
    DFTRACER_UTILS_NOINLINE Step continue_plain(std::uint32_t j,
                                                std::uint32_t start,
                                                std::uint32_t end,
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
    DFTRACER_UTILS_NOINLINE bool finish_block(std::uint32_t end_line);
    bool finish_open();
    DFTRACER_UTILS_NOINLINE void push_text(std::uint32_t owner,
                                           const detail::ScalarText& text,
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
    std::size_t depth_ = 0;
    std::uint32_t pending_ = NONE;
    std::uint32_t pending_col_ = 0;
    /// A value that ended: a deeper line is no part of it, and is an error.
    bool cont_active_ = false;
    /// The value that ended in `cont_active_` is a plain one that a deeper line
    /// may go on in; the scalar opens at that line.
    bool plain_pending_ = false;
    std::uint32_t cont_limit_ = 0;
    bool cont_strict_ = false;
    bool has_content_ = false;
    bool after_end_ = false;
    bool has_tab_ = false;
    std::uint32_t utf8_pending_ = 0;

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
    /// A block scalar, and the first line of its text.
    detail::BlockSpec block_;
    std::uint32_t block_first_ = 0;
    bool is_explicit_indent_ = false;
    bool has_text_ = false;
    std::uint32_t max_blank_ = 0;
    /// A flow collection: the last line that it holds, its entry in the side
    /// table, and the first Cont line, which names that entry in `next`.
    std::uint32_t flow_last_ = 0;
    std::uint32_t flow_index_ = 0;
    std::uint32_t flow_next_line_ = NONE;
    Level levels_[MAX_DEPTH];
    KeySet::Level level_keys_[MAX_DEPTH];
    KeySet keys_;
    FlowParser flow_;
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
    return false;
}

bool Builder::fail(std::uint32_t line, std::uint32_t offset,
                   const char* reason) {
    return fail(line, offset, dstd::String(reason));
}

bool Builder::fail_at(std::uint32_t offset, const char* reason) {
    return fail_at(offset, dstd::String(reason));
}

bool Builder::fail_at(std::uint32_t offset, dstd::String reason) {
    return detail::fail_at(ix_, offset, std::move(reason));
}

bool Builder::fail_escape(const detail::EscapeProblem& problem) {
    return fail_at(problem.at, detail::escape_reason(problem));
}

bool Builder::decode_into(std::uint32_t line, bool is_value, std::uint32_t s,
                          std::uint32_t e) {
    const bool dq = t_[s - 1] == '"';
    detail::EscapeProblem problem;
    detail::ScalarText text;
    if (detail::decode_quoted(ix_, s, e, dq, problem, text) !=
        detail::Decoded::Ok)
        return fail_escape(problem);
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

// False when the innermost map holds the key already. The set holds the keys
// before it.
bool Builder::note_key(std::uint32_t line, dstd::StringView key) {
    const bool in_text = key.data() >= t_ && key.data() < t_ + ix_.size;
    return keys_.add(level_keys_[depth_ - 1], depth_ - 1, line, key,
                     in_text ? t_ + ix_.size : key.data(),
                     [&](std::uint32_t other, dstd::StringView text) {
                         return same_key(other, text);
                     });
}

bool Builder::push_level(std::uint32_t col, std::uint32_t j, bool is_seq) {
    if (depth_ == MAX_DEPTH) [[unlikely]] {
        const std::uint32_t line = line_ - 1;
        return fail(line, ix_.entries[line].start + col,
                    "blocks nested deeper than 128 levels are not supported");
    }
    if (!is_seq) keys_.open(level_keys_[depth_]);
    Level& level = levels_[depth_++];
    level.cur = j;
    level.col = static_cast<std::uint16_t>(col);
    level.is_seq = is_seq;
    level.inline_seq = false;
    return true;
}

void Builder::pop_level(std::uint32_t at) {
    Level& level = levels_[depth_ - 1];
    if (level.cur != NONE) ix_.next[level.cur] = at;
    if (!level.is_seq) keys_.close(level_keys_[depth_ - 1], depth_ - 1);
    --depth_;
}

void Builder::resolve_pending(bool child) {
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
            case LineKind::NestItem:
                e.kind = LineKind::NestItemOpen;
                break;
            case LineKind::NestMap:
                e.kind = LineKind::NestMapOpen;
                break;
            default:
                break;
        }
    }
    pending_ = NONE;
}

bool Builder::marker(std::uint32_t j) {
    resolve_pending(false);
    while (depth_ > 0) pop_level(j);
    cont_active_ = false;
    plain_pending_ = false;
    ix_.has_markers = true;
    has_content_ = false;
    after_end_ = false;
    return true;
}

// Opens the block scalar of the header in `pt`, whose lines start at
// `first_line`.
void Builder::begin_block(const LineParts& pt, std::uint32_t first_line) {
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
    block_.indent = static_cast<std::uint32_t>(
        std::max<std::int32_t>(limit_ + static_cast<std::int32_t>(digit), 0));
    block_first_ = first_line;
    max_blank_ = 0;
    open_ = Open::Block;
}

// Reads the flow collection of the owner `owner`, which starts at `pt.open_at`
// in the line `j`: the flow parser reads it, the lines after the first are Cont
// and the line of its end holds only a comment after it.
bool Builder::begin_flow(std::uint32_t owner, std::uint32_t j,
                         const LineParts& pt) {
    std::uint32_t end = 0;
    std::uint32_t root = 0;
    const std::uint32_t min_indent =
        limit_ < 0 ? 0u : static_cast<std::uint32_t>(limit_) + 1;
    if (!flow_.parse(pt.open_at, min_indent, end, root)) return false;
    const void* newline = std::memchr(t_ + end, '\n', ix_.size - end);
    std::uint32_t eol = newline == nullptr
                            ? ix_.size
                            : static_cast<std::uint32_t>(
                                  static_cast<const char*>(newline) - t_);
    if (eol > end && t_[eol - 1] == '\r') --eol;
    std::uint32_t after = end;
    while (after < eol && is_white(t_[after])) ++after;
    if (after < eol && !(t_[after] == '#' && after > end)) {
        if (is_value_sep(t_, after, eol)) {
            const bool is_key_position =
                pt.kind == LineKind::Root || pt.kind == LineKind::RootDoc ||
                pt.kind == LineKind::SeqItem || pt.kind == LineKind::NestItem;
            return fail_at(after, is_key_position
                                      ? "a collection cannot be a key"
                                      : "mapping values are not allowed in "
                                        "this context");
        }
        return fail_at(after, "unexpected text after a flow collection");
    }
    const std::uint32_t last =
        j + static_cast<std::uint32_t>(
                detail::count_newlines(t_ + pt.open_at, end - pt.open_at));
    LineEntry& entry = ix_.entries[owner];
    entry.flags = static_cast<std::uint8_t>(
        (entry.flags & ~line_flag::VAL_EMPTY) | line_flag::FLOW);
    const auto index = static_cast<std::uint32_t>(ix_.escapes.size());
    ix_.escapes.push_back(EscapeEntry{owner * 2 + 1, root, 0, last + 1});
    if (last > owner && owner + 1 <= j) ix_.next[owner + 1] = index;
    if (last > j) {
        open_ = Open::Flow;
        flow_last_ = last;
        flow_index_ = index;
        flow_next_line_ = owner + 1 > j ? owner + 1 : NONE;
    }
    complete_value();
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
        begin_block(pt, j + 1);
        return true;
    }
    if (pt.open == LineOpen::Quoted) {
        open_ = Open::Quoted;
        quote_at_ = pt.open_at;
        bad_line_ = NONE;
        return true;
    }
    if (pt.open == LineOpen::Flow) return begin_flow(j, j, pt);
    if ((pt.flags & (line_flag::VAL_QUOTED | line_flag::COMMENT)) == 0) {
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
    owner_ = pending_;
    first_line_ = j;
    limit_ = static_cast<std::int32_t>(pending_col_);
    LineEntry& owner = ix_.entries[owner_];
    is_strict_ =
        owner.kind != LineKind::SeqItem && owner.kind != LineKind::NestItem;
    owner.flags =
        static_cast<std::uint8_t>(owner.flags & ~line_flag::VAL_EMPTY);
    pending_ = NONE;
    for (std::uint32_t l = owner_ + 1; l <= j; ++l)
        set_cont(ix_.entries[l], ix_.entries[l].flags & line_flag::CR);
    if (pt.open == LineOpen::Flow) return begin_flow(owner_, j, pt);
    owner.flags |= line_flag::VAL_ESC;
    if (pt.open == LineOpen::Block) {
        owner.flags |= line_flag::VAL_QUOTED;
        begin_block(pt, j + 1);
        return true;
    }
    if (pt.open == LineOpen::Quoted) {
        owner.flags |= line_flag::VAL_QUOTED;
        open_ = Open::Quoted;
        quote_at_ = pt.open_at;
        bad_line_ = NONE;
        return true;
    }
    if ((pt.flags & line_flag::VAL_QUOTED) != 0) {
        owner.flags |= line_flag::VAL_QUOTED;
        detail::ScalarText text;
        text.in_text = true;
        text.off = pt.val_s;
        text.len = pt.val_e - pt.val_s;
        if ((pt.flags & line_flag::VAL_ESC) != 0) {
            detail::EscapeProblem problem;
            if (detail::decode_quoted(ix_, pt.val_s, pt.val_e,
                                      t_[pt.val_s - 1] == '"', problem,
                                      text) != detail::Decoded::Ok)
                return fail_escape(problem);
        }
        push_text(owner_, text, j + 1);
        complete_value();
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
        case Open::Block:
            return block_line(j, start, end, cr);
        default:
            set_cont(ix_.entries[j], cr);
            if (j == flow_next_line_) ix_.next[j] = flow_index_;
            if (j == flow_last_) open_ = Open::None;
            return Step::Consumed;
    }
}

bool Builder::finish_plain() {
    open_ = Open::None;
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
    std::uint32_t c = i;
    while (c < end && is_white(t_[c])) ++c;
    LineEntry& entry = ix_.entries[j];
    if (c == end) {
        if (n > INDENT_LIMIT) {
            fail(j, i,
                 "indentation deeper than 65534 columns is not "
                 "supported");
            return Step::Fail;
        }
        entry.indent = static_cast<std::uint16_t>(n);
        entry.kind = LineKind::Blank;
        entry.flags = cr;
        return Step::Consumed;
    }
    if ((limit_ >= 0 && n <= static_cast<std::uint32_t>(limit_)) ||
        (n == 0 && is_marker(t_, i, end)) || t_[c] == '#')
        return finish_plain() ? Step::Pass : Step::Fail;
    const PlainScan scan = scan_plain(t_, c, end);
    if (scan.sep != NONE) {
        fail(j, scan.sep,
             "mapping values are not allowed in a multi-line plain scalar");
        return Step::Fail;
    }
    std::uint32_t te = scan.comment != NONE ? scan.comment : end;
    while (te > c && is_white(t_[te - 1])) --te;
    set_cont(entry, cr);
    last_ = j;
    last_e_ = te;
    if (scan.comment != NONE)
        return finish_plain() ? Step::Consumed : Step::Fail;
    return Step::Consumed;
}

Step Builder::quoted_line(std::uint32_t j, std::uint32_t start,
                          std::uint32_t end, std::uint8_t cr) {
    std::uint32_t i = start;
    while (i < end && t_[i] == ' ') ++i;
    const std::uint32_t n = i - start;
    set_cont(ix_.entries[j], cr);
    std::uint32_t c = i;
    while (c < end && is_white(t_[c])) ++c;
    if (c == end) return Step::Consumed;
    if (limit_ >= 0 && n <= static_cast<std::uint32_t>(limit_)) {
        note_bad(j, c, "a quoted scalar is not indented enough");
    } else if (n == 0 && is_marker(t_, i, end)) {
        note_bad(j, i, "a document marker inside a quoted scalar");
    }
    const std::uint32_t close = find_close(t_, c, end, t_[quote_at_]);
    if (close == NONE) return Step::Consumed;
    if (bad_line_ != NONE) {
        fail(bad_line_, bad_at_, bad_reason_);
        return Step::Fail;
    }
    bool has_comment = false;
    std::uint32_t stray = 0;
    if (!ends_in_comment_or_nothing(t_, close + 1, end, has_comment, stray)) {
        fail(j, stray, "unexpected text after a quoted scalar");
        return Step::Fail;
    }
    detail::EscapeProblem problem;
    detail::ScalarText text;
    for (std::uint32_t l = owner_ + 1; l < first_line_; ++l)
        ix_.entries[l].kind = LineKind::Cont;
    if (detail::decode_quoted(ix_, quote_at_ + 1, close, t_[quote_at_] == '"',
                              problem, text) != detail::Decoded::Ok) {
        fail_escape(problem);
        return Step::Fail;
    }
    open_ = Open::None;
    push_text(owner_, text, j + 1);
    complete_value();
    return Step::Consumed;
}

bool Builder::finish_block(std::uint32_t end_line) {
    open_ = Open::None;
    detail::ScalarText text;
    detail::decode_block(ix_, block_, block_first_, end_line, text);
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
        if (!block_.is_indent_known) {
            if (static_cast<std::int64_t>(spaces) <= limit_) {
                std::uint32_t k = i;
                while (k < end && is_white(t_[k])) ++k;
                if (t_[i] == '\t' && k == end) {
                    fail(j, i, "a tab cannot be used as indentation");
                    return Step::Fail;
                }
                return finish_block(j) ? Step::Pass : Step::Fail;
            }
            block_.indent = spaces;
            block_.is_indent_known = true;
            if (max_blank_ > spaces) {
                for (std::uint32_t l = block_first_; l < j; ++l) {
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

// A line deeper than the value that ended before it is an error: a value does
// not go on in a deeper line.
bool Builder::deeper_line(std::uint32_t j, const LineParts& pt) {
    return fail(j, ix_.entries[j].start + pt.indent,
                "a complete value cannot go on in a deeper line");
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
    const std::uint32_t line_start = ix_.entries[j].start;
    if (after_end_) [[unlikely]]
        return fail(j, line_start + pt.indent,
                    "a document has one root value, and this is another");
    const std::uint32_t n = pt.indent;
    const bool is_root =
        pt.kind == LineKind::Root || pt.kind == LineKind::RootDoc;
    if (is_root && pending_ != NONE && n > pending_col_) [[unlikely]]
        return begin_next_line(j, pt);
    const bool had_content = has_content_;
    has_content_ = true;
    cont_active_ = false;
    plain_pending_ = false;
    if (is_root) [[unlikely]] {
        if (depth_ != 0 || had_content)
            return fail(j, line_start + n,
                        "a scalar line needs a mapping key or a sequence "
                        "entry");
        after_end_ = true;
        return begin_scalar(j, pt, -1, false);
    }
    const bool is_seq = is_seq_kind(pt.kind);
    const bool child = pending_ != NONE && n > pending_col_;
    bool inline_seq = false;
    if (pending_ != NONE && n == pending_col_ && is_seq) {
        const LineKind pending_kind = ix_.entries[pending_].kind;
        inline_seq = pending_kind == LineKind::MapEntry ||
                     pending_kind == LineKind::SeqMap ||
                     pending_kind == LineKind::NestMap;
    }
    resolve_pending(child || inline_seq);
    if (depth_ == 0) {
        if (!push_level(n, j, is_seq)) return false;
    } else if (child) {
        if (!push_level(n, j, is_seq)) return false;
    } else if (inline_seq) {
        if (!push_level(n, j, true)) return false;
        levels_[depth_ - 1].inline_seq = true;
    } else {
        while (depth_ > 0 && levels_[depth_ - 1].col > n) pop_level(j);
        if (depth_ > 1 && levels_[depth_ - 1].inline_seq &&
            levels_[depth_ - 1].col == n && !is_seq)
            pop_level(j);
        if (depth_ == 0 || levels_[depth_ - 1].col != n) [[unlikely]]
            return fail(j, line_start + n,
                        "indentation does not match any open block");
        Level& top = levels_[depth_ - 1];
        if (top.is_seq != is_seq) [[unlikely]]
            return fail(j, line_start + n,
                        is_seq ? "a sequence item cannot follow mapping keys "
                                 "at the same indentation"
                               : "a mapping key cannot follow sequence items "
                                 "at the same indentation");
        if (top.cur != NONE) ix_.next[top.cur] = j;
        top.cur = j;
    }
    std::uint32_t limit = n;
    if (pt.nest != 0) [[unlikely]] {
        std::uint32_t p = pt.dash;
        for (std::uint32_t d = 0; d < pt.nest; ++d) {
            ++p;
            while (is_white(t_[p])) ++p;
            const std::uint32_t col = p - line_start;
            if (col > INDENT_LIMIT)
                return fail(j, p,
                            "indentation deeper than 65534 columns is not "
                            "supported");
            if (!push_level(col, NONE, true)) return false;
            limit = col;
        }
    }
    if (pt.kind == LineKind::SeqMap || pt.kind == LineKind::NestMap) {
        const std::uint32_t map_col = pt.content - line_start;
        if (map_col > INDENT_LIMIT)
            return fail(j, pt.content,
                        "indentation deeper than 65534 columns is not "
                        "supported");
        if (!push_level(map_col, NONE, false)) return false;
        limit = map_col;
    }
    if (pt.kind == LineKind::MapEntry || pt.kind == LineKind::SeqMap ||
        pt.kind == LineKind::NestMap) {
        const dstd::StringView key = key_view(j, pt);
        if (!note_key(j, key)) [[unlikely]] {
            const std::uint32_t quoted =
                (pt.flags & line_flag::KEY_QUOTED) != 0 ? 1u : 0u;
            return fail(j, pt.key_s - quoted,
                        dstd::fmt<"duplicate key '{}'">(key));
        }
    }
    if (pt.kind == LineKind::MapEntry || pt.kind == LineKind::SeqMap ||
        pt.kind == LineKind::SeqItem || pt.kind == LineKind::NestItem ||
        pt.kind == LineKind::NestMap) {
        const bool is_strict =
            pt.kind != LineKind::SeqItem && pt.kind != LineKind::NestItem;
        if ((pt.flags & line_flag::VAL_EMPTY) != 0) {
            pending_ = j;
            pending_col_ = limit;
        } else if (((pt.flags & (line_flag::VAL_QUOTED | line_flag::COMMENT)) |
                    pt.open) == 0) {
            // A plain value: a deeper line may go on in it, and the scalar
            // opens there.
            owner_ = j;
            cont_active_ = true;
            cont_limit_ = limit;
            cont_strict_ = is_strict;
            plain_pending_ = true;
        } else {
            return begin_scalar(j, pt, static_cast<std::int32_t>(limit),
                                is_strict);
        }
    }
    return true;
}

// The line ends at `nl_pos`. A plain line in an open block goes the short way;
// every other line, and every error, goes to process_any_line.
DFTRACER_UTILS_ALWAYS_INLINE inline bool Builder::process_line(
    std::uint32_t start, std::uint32_t nl_pos) {
    if ((static_cast<unsigned>(open_) | static_cast<unsigned>(has_tab_)) != 0 ||
        batch_ == nullptr || start < batch_->base) [[unlikely]]
        return process_any_line(start, nl_pos);
    std::uint32_t end = nl_pos;
    std::uint8_t cr = 0;
    if (end > start && t_[end - 1] == '\r') {
        --end;
        cr = line_flag::CR;
    }
    PlainLine pl;
    if (!classify_plain(*batch_, t_, start, end, pl)) [[unlikely]]
        return process_any_line(start, nl_pos);
    if (pl.kind == LineKind::Blank) {
        if (plain_pending_) [[unlikely]]
            return process_any_line(start, nl_pos);
    } else if ((cont_active_ && pl.indent > cont_limit_) || after_end_ ||
               depth_ == 0) [[unlikely]] {
        return process_any_line(start, nl_pos);
    }
    const std::uint32_t j = line_++;
    LineEntry& entry = ix_.entries[j];
    entry.start = start;
    entry.indent = static_cast<std::uint16_t>(pl.indent);
    entry.kind = pl.kind;
    entry.flags = static_cast<std::uint8_t>(cr | pl.flags);
    ix_.entries[j + 1].start = nl_pos + 1;
    ix_.next[j] = line_count_;
    if (pl.kind == LineKind::Blank) return true;
    return structure_plain(j, start, pl);
}

// structure for a line that classify_plain read, in a block that is open: no
// root, no escape, no nested dash, and a value that is empty or plain.
DFTRACER_UTILS_ALWAYS_INLINE inline bool Builder::structure_plain(
    std::uint32_t j, std::uint32_t line_start, const PlainLine& pl) {
    const std::uint32_t n = pl.indent;
    has_content_ = true;
    cont_active_ = false;
    plain_pending_ = false;
    const bool is_seq =
        pl.kind == LineKind::SeqItem || pl.kind == LineKind::SeqMap;
    if (pending_ != NONE) {
        const bool child = n > pending_col_;
        bool inline_seq = false;
        if (n == pending_col_ && is_seq) {
            const LineKind pending_kind = ix_.entries[pending_].kind;
            inline_seq = pending_kind == LineKind::MapEntry ||
                         pending_kind == LineKind::SeqMap ||
                         pending_kind == LineKind::NestMap;
        }
        resolve_pending(child || inline_seq);
        if (child) {
            if (!push_level(n, j, is_seq)) return false;
            return plain_key_value(j, line_start, pl, n);
        }
        if (inline_seq) {
            if (!push_level(n, j, true)) return false;
            levels_[depth_ - 1].inline_seq = true;
            return plain_key_value(j, line_start, pl, n);
        }
    }
    while (depth_ > 0 && levels_[depth_ - 1].col > n) pop_level(j);
    if (depth_ > 1 && levels_[depth_ - 1].inline_seq &&
        levels_[depth_ - 1].col == n && !is_seq)
        pop_level(j);
    if (depth_ == 0 || levels_[depth_ - 1].col != n) [[unlikely]]
        return fail(j, line_start + n,
                    "indentation does not match any open block");
    Level& top = levels_[depth_ - 1];
    if (top.is_seq != is_seq) [[unlikely]]
        return fail(j, line_start + n,
                    is_seq ? "a sequence item cannot follow mapping keys at "
                             "the same indentation"
                           : "a mapping key cannot follow sequence items at "
                             "the same indentation");
    if (top.cur != NONE) ix_.next[top.cur] = j;
    top.cur = j;
    return plain_key_value(j, line_start, pl, n);
}

// The map of a `- k: v` item, the key and the value of a plain line, whose
// block starts at column `n`.
DFTRACER_UTILS_ALWAYS_INLINE inline bool Builder::plain_key_value(
    std::uint32_t j, std::uint32_t line_start, const PlainLine& pl,
    std::uint32_t n) {
    std::uint32_t limit = n;
    if (pl.kind == LineKind::SeqMap) {
        limit = pl.content - line_start;
        if (!push_level(limit, NONE, false)) return false;
    }
    if (pl.kind != LineKind::SeqItem) {
        const dstd::StringView key(t_ + pl.key_s, pl.key_e - pl.key_s);
        if (!keys_.add(level_keys_[depth_ - 1], depth_ - 1, j, key,
                       t_ + ix_.size,
                       [&](std::uint32_t other, dstd::StringView text) {
                           return same_key(other, text);
                       })) [[unlikely]]
            return duplicate_key(j, pl.key_s, key);
    }
    if ((pl.flags & line_flag::VAL_EMPTY) != 0) {
        pending_ = j;
        pending_col_ = limit;
    } else {
        // A plain value: a deeper line may go on in it, and the scalar opens
        // there.
        owner_ = j;
        cont_active_ = true;
        cont_limit_ = limit;
        cont_strict_ = pl.kind != LineKind::SeqItem;
        plain_pending_ = true;
    }
    return true;
}

bool Builder::duplicate_key(std::uint32_t j, std::uint32_t at,
                            dstd::StringView key) {
    return fail(j, at, dstd::fmt<"duplicate key '{}'">(key));
}

// Reads a line of any kind: open scalars, TABs, escapes, roots, markers and
// errors.
bool Builder::process_any_line(std::uint32_t start, std::uint32_t nl_pos) {
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
    bool has_tab = false;
    if ((static_cast<unsigned>(open_) | static_cast<unsigned>(has_tab_)) != 0)
        [[unlikely]] {
        if (open_ != Open::None) {
            const Step step = open_line(j, start, end, flags);
            if (step == Step::Consumed) return true;
            if (step == Step::Fail) return false;
        }
        has_tab =
            has_tab_ && std::memchr(t_ + start, '\t', end - start) != nullptr;
    }
    LineParts pt;
    PlainLine pl;
    const bool plain = !has_tab && batch_ != nullptr && start >= batch_->base &&
                       classify_plain(*batch_, t_, start, end, pl);
    if (plain) {
        pt.indent = pl.indent;
        pt.content = pl.content;
        pt.key_s = pl.key_s;
        pt.key_e = pl.key_e;
        pt.kind = pl.kind;
        pt.flags = pl.flags;
    }
    if (!plain && !classify_line(t_, start, end, pt)) {
        if (plain_pending_ && pt.indent > cont_limit_) {
            const Step step = continue_plain(j, start, end, flags);
            if (step == Step::Consumed) return true;
            if (step == Step::Fail) return false;
        }
        // A line that goes on a value that ended may hold any text.
        if (cont_active_ && pt.indent > cont_limit_ &&
            pt.indent <= INDENT_LIMIT && start + pt.indent != end)
            return fail(j, start + pt.indent,
                        "a complete value cannot go on in a deeper line");
        return fail(j, pt.error_at, pt.reason);
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
            return marker(j);
        case LineKind::RootDoc:
            if (!marker(j)) return false;
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
        // The value of a line that is the value of an earlier entry is
        // decoded with that entry.
        const bool is_next_line_value = pt.kind == LineKind::Root &&
                                        pending_ != NONE &&
                                        pt.indent > pending_col_;
        if ((pt.flags & line_flag::KEY_ESC) != 0 &&
            !decode_into(j, false, pt.key_s, pt.key_e))
            return false;
        if ((pt.flags & line_flag::VAL_ESC) != 0 && pt.open == LineOpen::NONE &&
            !is_next_line_value && !decode_into(j, true, pt.val_s, pt.val_e))
            return false;
    }
    return structure(j, pt);
}

bool Builder::build(std::size_t size) {
    const auto text_size = static_cast<std::uint32_t>(size);
    const bool ends_in_newline = t_[size - 1] == '\n';
    line_count_ = static_cast<std::uint32_t>(detail::count_newlines(t_, size) +
                                             (ends_in_newline ? 0 : 1));
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
            if (high[b] != 0 || utf8_pending_ != 0) {
                const auto to = static_cast<std::uint32_t>(
                    std::min<std::size_t>(base + 64, size));
                const std::uint32_t at =
                    find_bad_utf8(t_, base, to, text_size, utf8_pending_);
                if (at != NONE)
                    return fail_at(at,
                                   "invalid UTF-8 or a character that YAML "
                                   "does not print");
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
            // A TAB is white space, so the lines decide.
            for (std::size_t b = 0; b < blocks; ++b) {
                std::uint64_t m = bad[b];
                while (m != 0) {
                    const std::size_t at =
                        off + b * 64 +
                        static_cast<std::size_t>(dstd::bits::ctz_u64(m));
                    if (t_[at] != '\t')
                        return fail_at(static_cast<std::uint32_t>(at),
                                       "a control character is not allowed");
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
    if (!finish_open()) return false;
    resolve_pending(false);
    ix_.entries[line_count_].indent = 0;
    ix_.entries[line_count_].kind = LineKind::Blank;
    ix_.entries[line_count_].flags = 0;
    return true;
}

}  // namespace

void LineIndex::release() {
    entries.release();
    tape.release();
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

bool fail_at(LineIndex& ix, std::uint32_t offset, dstd::String reason) {
    const char* t = ix.text;
    std::uint32_t line_start = 0;
    std::uint32_t lines = 0;
    for (std::uint32_t at = 0; at < offset;) {
        const void* nl = std::memchr(t + at, '\n', offset - at);
        if (nl == nullptr) break;
        line_start =
            static_cast<std::uint32_t>(static_cast<const char*>(nl) - t) + 1;
        at = line_start;
        ++lines;
    }
    ix.problem.line = lines + 1;
    ix.problem.col = offset - line_start + 1;
    ix.problem.reason = std::move(reason);
    return false;
}

}  // namespace detail

BuildResult LineIndex::build(const char* text_in, std::size_t size_in) {
    text = text_in;
    size = 0;
    line_count = 0;
    has_markers = false;
    arena.clear();
    escapes.clear();
    entries.clear();
    tape.clear();
    next.clear();
    problem = LineProblem{};
    if (size_in >= TEXT_SIZE_LIMIT) {
        problem.line = 1;
        problem.col = 1;
        problem.reason = "a text of 4 GiB or more is not supported";
        return BuildResult::Invalid;
    }
    size = static_cast<std::uint32_t>(size_in);
    if (size_in == 0) {
        entries.resize(1);
        next.resize(1);
        entries[0] = LineEntry{0, 0, LineKind::Blank, 0};
        next[0] = 0;
        return BuildResult::Accepted;
    }
    Builder builder(*this);
    return builder.build(size_in) ? BuildResult::Accepted
                                  : BuildResult::Invalid;
}

}  // namespace dftracer::utils::yaml
