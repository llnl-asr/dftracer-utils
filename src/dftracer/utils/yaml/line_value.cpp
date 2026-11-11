// SPDX-License-Identifier: MIT
#include <dftracer/utils/yaml/line_value.h>
#include <dftracer/utils/yaml/scalar.h>

#include <algorithm>
#include <cstring>

namespace dftracer::utils::yaml {

namespace {

constexpr std::uint8_t ARENA_BIT = 0x80;
constexpr std::uint8_t TAG_MASK = 0x03;

bool is_skipped(LineKind kind) {
    return kind == LineKind::Blank || kind == LineKind::Comment;
}

bool is_seq_kind(LineKind kind) {
    return kind == LineKind::SeqItem || kind == LineKind::SeqOpen ||
           kind == LineKind::SeqMap || kind == LineKind::SeqMapOpen;
}

std::uint64_t pack(std::uint32_t lo, std::uint32_t hi) {
    return lo | (static_cast<std::uint64_t>(hi) << 32);
}

LineTag tag_of(std::uint8_t tag) {
    return static_cast<LineTag>(tag & TAG_MASK);
}

dstd::StringView escaped(const LineIndex& ix, std::uint32_t line,
                         bool is_value) {
    const std::uint32_t key = line * 2 + (is_value ? 1u : 0u);
    const auto it = std::lower_bound(
        ix.escapes.begin(), ix.escapes.end(), key,
        [](const EscapeEntry& e, std::uint32_t k) { return e.key < k; });
    return dstd::StringView(ix.arena.data() + it->off, it->len);
}

dstd::StringView key_of(const LineIndex& ix, std::uint32_t line,
                        const LineParts& pt) {
    if ((pt.flags & line_flag::KEY_ESC) != 0) return escaped(ix, line, false);
    return dstd::StringView(ix.text + pt.key_s, pt.key_e - pt.key_s);
}

// The first line after `line` that is not a blank or a comment.
std::uint32_t content_after(const LineIndex& ix, std::uint32_t line) {
    std::uint32_t c = line + 1;
    while (is_skipped(ix.entries[c].kind)) ++c;
    return c;
}

constexpr std::uint8_t NOT_PLAIN =
    line_flag::KEY_QUOTED | line_flag::VAL_QUOTED | line_flag::FLOW_EMPTY |
    line_flag::COMMENT | line_flag::KEY_ESC | line_flag::VAL_ESC;

// The facts of a line. A line that holds no quote, comment or empty flow is
// read by a short scan, and any other by classify_line, which stage 1 ran on
// it already.
void parts_of(const LineIndex& ix, std::uint32_t line, LineParts& pt) {
    const LineEntry& e = ix.entries[line];
    const std::uint32_t end = ix.line_end(line);
    if ((e.flags & NOT_PLAIN) != 0 || e.kind == LineKind::Blank ||
        e.kind == LineKind::Comment) {
        classify_line(ix.text, e.start, end, ix.size, pt);
        return;
    }
    const char* t = ix.text;
    std::uint32_t p = e.start + e.indent;
    const bool seq = is_seq_kind(e.kind);
    if (seq) {
        ++p;
        while (p < end && t[p] == ' ') ++p;
    }
    pt.kind = e.kind;
    pt.flags = e.flags & line_flag::VAL_EMPTY;
    pt.indent = e.indent;
    pt.content = p;
    if (e.kind == LineKind::SeqItem || e.kind == LineKind::SeqOpen) {
        pt.val_s = p;
        std::uint32_t ve = end;
        while (ve > p && t[ve - 1] == ' ') --ve;
        pt.val_e = ve;
        return;
    }
    std::uint32_t k = p;
    while (!(t[k] == ':' && (k + 1 == end || t[k + 1] == ' '))) ++k;
    std::uint32_t ke = k;
    while (ke > p && t[ke - 1] == ' ') --ke;
    pt.key_s = p;
    pt.key_e = ke;
    std::uint32_t v = k + 1;
    while (v < end && t[v] == ' ') ++v;
    pt.val_s = v;
    std::uint32_t ve = end;
    while (ve > v && t[ve - 1] == ' ') --ve;
    pt.val_e = ve;
}

// Calls `f(line)` for each member line of the map that starts at `first`,
// until it returns false.
template <class F>
void each_member_line(const LineIndex& ix, std::uint32_t first,
                      std::uint32_t limit, std::uint32_t col, F&& f) {
    if (first >= limit) return;
    std::uint32_t k = first;
    const LineKind first_kind = ix.entries[first].kind;
    if (!f(k)) return;
    if (first_kind == LineKind::SeqMap || first_kind == LineKind::SeqMapOpen) {
        std::uint32_t c = k + 1;
        while (c < limit && is_skipped(ix.entries[c].kind)) ++c;
        while (c < limit && (ix.entries[c].indent > col ||
                             (ix.entries[c].indent == col &&
                              is_seq_kind(ix.entries[c].kind))))
            c = ix.next[c];
        k = c;
    } else {
        k = ix.next[k];
    }
    while (k < limit && ix.entries[k].indent == col) {
        if (!f(k)) return;
        k = ix.next[k];
    }
}

}  // namespace

Value LineAccess::container(const DocumentStorage* storage, std::uint32_t first,
                            std::uint32_t limit) {
    const LineEntry& e = storage->lines.entries[first];
    const LineTag tag = is_seq_kind(e.kind) ? LineTag::Seq : LineTag::Map;
    return Value(storage, pack(first, limit), e.indent,
                 static_cast<std::uint8_t>(tag));
}

Value LineAccess::make(const DocumentStorage* storage, LineTag tag,
                       std::uint64_t id, std::uint32_t col, bool arena) {
    return Value(storage, id, col,
                 static_cast<std::uint8_t>(static_cast<std::uint8_t>(tag) |
                                           (arena ? ARENA_BIT : 0)));
}

namespace {

// The block under the Open line `line`.
Value child_block(const DocumentStorage* storage, std::uint32_t line) {
    const LineIndex& ix = storage->lines;
    return LineAccess::container(storage, content_after(ix, line),
                                 ix.next[line]);
}

// The value that the line's own text holds.
Value inline_value(const DocumentStorage* storage, std::uint32_t line,
                   const LineParts& pt) {
    const LineIndex& ix = storage->lines;
    if ((pt.flags & line_flag::FLOW_EMPTY) != 0)
        return LineAccess::make(
            storage, ix.text[pt.val_s] == '{' ? LineTag::Map : LineTag::Seq, 0,
            0);
    if ((pt.flags & line_flag::VAL_EMPTY) != 0)
        return LineAccess::make(storage, LineTag::Null, 0, 0);
    if ((pt.flags & line_flag::VAL_QUOTED) != 0) {
        if ((pt.flags & line_flag::VAL_ESC) != 0) {
            const dstd::StringView text = escaped(ix, line, true);
            const auto off =
                static_cast<std::uint32_t>(text.data() - ix.arena.data());
            return LineAccess::make(
                storage, LineTag::Scalar,
                pack(off, off + static_cast<std::uint32_t>(text.size())), 0,
                true);
        }
        return LineAccess::make(storage, LineTag::Scalar,
                                pack(pt.val_s, pt.val_e), 0);
    }
    if (is_null_text(dstd::StringView(ix.text + pt.val_s, pt.val_e - pt.val_s)))
        return LineAccess::make(storage, LineTag::Null, 0, 0);
    return LineAccess::make(storage, LineTag::Scalar, pack(pt.val_s, pt.val_e),
                            0);
}

Value member_value(const DocumentStorage* storage, std::uint32_t line,
                   const LineParts& pt) {
    const LineKind kind = storage->lines.entries[line].kind;
    if (kind == LineKind::MapOpen || kind == LineKind::SeqOpen ||
        kind == LineKind::SeqMapOpen)
        return child_block(storage, line);
    return inline_value(storage, line, pt);
}

}  // namespace

Value LineAccess::element(const DocumentStorage* storage, std::uint32_t line) {
    const LineIndex& ix = storage->lines;
    const LineEntry& e = ix.entries[line];
    if (e.kind == LineKind::SeqMap || e.kind == LineKind::SeqMapOpen) {
        std::uint32_t p = e.start + e.indent + 1;
        while (ix.text[p] == ' ') ++p;
        return Value(storage, pack(line, ix.next[line]), p - e.start,
                     static_cast<std::uint8_t>(LineTag::Map));
    }
    LineParts pt;
    parts_of(ix, line, pt);
    return member_value(storage, line, pt);
}

bool LineAccess::next_document(const DocumentStorage* storage,
                               std::uint32_t& pos, std::uint32_t& first,
                               std::uint32_t& limit) {
    const LineIndex& ix = storage->lines;
    std::uint32_t k = pos;
    while (k < ix.line_count) {
        const LineKind kind = ix.entries[k].kind;
        if (!is_skipped(kind) && kind != LineKind::DocStart &&
            kind != LineKind::DocEnd)
            break;
        ++k;
    }
    if (k >= ix.line_count) return false;
    first = k;
    std::uint32_t l = k + 1;
    if (ix.has_markers) {
        while (l < ix.line_count && ix.entries[l].kind != LineKind::DocStart &&
               ix.entries[l].kind != LineKind::DocEnd)
            ++l;
    } else {
        l = ix.line_count;
    }
    limit = l;
    pos = l;
    return true;
}

Value LineAccess::root(const DocumentStorage* storage) {
    std::uint32_t pos = 0;
    std::uint32_t first = 0;
    std::uint32_t limit = 0;
    if (!next_document(storage, pos, first, limit))
        return Value(storage, 0, 0, static_cast<std::uint8_t>(LineTag::Null));
    return container(storage, first, limit);
}

bool LineAccess::is_object(const Value& v) {
    return tag_of(v.tag_) == LineTag::Map;
}
bool LineAccess::is_array(const Value& v) {
    return tag_of(v.tag_) == LineTag::Seq;
}
bool LineAccess::is_string(const Value& v) {
    return tag_of(v.tag_) == LineTag::Scalar;
}
bool LineAccess::is_null(const Value& v) {
    return tag_of(v.tag_) == LineTag::Null;
}

dstd::StringView LineAccess::raw(const Value& v) {
    if (tag_of(v.tag_) != LineTag::Scalar) return {};
    const LineIndex& ix = v.storage_->lines;
    const char* base = (v.tag_ & ARENA_BIT) != 0 ? ix.arena.data() : ix.text;
    const auto s = static_cast<std::uint32_t>(v.id_);
    const auto e = static_cast<std::uint32_t>(v.id_ >> 32);
    return dstd::StringView(base + s, e - s);
}

dstd::Optional<Value> LineAccess::find(const Value& v, dstd::StringView key) {
    if (!is_object(v)) return dstd::nullopt;
    const DocumentStorage* storage = v.storage_;
    const LineIndex& ix = storage->lines;
    const char* t = ix.text;
    const auto first = static_cast<std::uint32_t>(v.id_);
    const auto limit = static_cast<std::uint32_t>(v.id_ >> 32);
    dstd::Optional<Value> out;
    const bool is_plain_key = key.find(':') == dstd::StringView::npos;
    each_member_line(ix, first, limit, v.col_, [&](std::uint32_t k) {
        const std::uint32_t sc = ix.entries[k].start + v.col_;
        const char c = t[sc];
        if (c != '"' && c != '\'' && !key.empty()) {
            if (c != key[0]) return true;
            const std::uint32_t end = ix.line_end(k);
            if (sc + key.size() >= end ||
                std::memcmp(t + sc, key.data(), key.size()) != 0)
                return true;
            const char after = t[sc + key.size()];
            if (after != ':' && after != ' ') return true;
            const LineEntry& e = ix.entries[k];
            const std::uint32_t colon =
                sc + static_cast<std::uint32_t>(key.size());
            if (after == ':' && is_plain_key && (e.flags & NOT_PLAIN) == 0 &&
                (e.kind == LineKind::MapEntry || e.kind == LineKind::SeqMap) &&
                (colon + 1 == end || t[colon + 1] == ' ')) {
                LineParts pt;
                pt.kind = e.kind;
                pt.flags = 0;
                std::uint32_t vs = colon + 1;
                while (vs < end && t[vs] == ' ') ++vs;
                std::uint32_t ve = end;
                while (ve > vs && t[ve - 1] == ' ') --ve;
                pt.val_s = vs;
                pt.val_e = ve;
                out = member_value(storage, k, pt);
                return false;
            }
        }
        LineParts pt;
        parts_of(ix, k, pt);
        if (key_of(ix, k, pt) != key) return true;
        out = member_value(storage, k, pt);
        return false;
    });
    return out;
}

void LineAccess::for_each_member(
    const Value& v, dstd::FunctionRef<void(dstd::StringView, Value)> fn) {
    if (!is_object(v)) return;
    const DocumentStorage* storage = v.storage_;
    const LineIndex& ix = storage->lines;
    const auto first = static_cast<std::uint32_t>(v.id_);
    const auto limit = static_cast<std::uint32_t>(v.id_ >> 32);
    each_member_line(ix, first, limit, v.col_, [&](std::uint32_t k) {
        LineParts pt;
        parts_of(ix, k, pt);
        fn(key_of(ix, k, pt), member_value(storage, k, pt));
        return true;
    });
}

void LineAccess::for_each_element(const Value& v,
                                  dstd::FunctionRef<void(Value)> fn) {
    if (!is_array(v)) return;
    const DocumentStorage* storage = v.storage_;
    const LineIndex& ix = storage->lines;
    const auto limit = static_cast<std::uint32_t>(v.id_ >> 32);
    std::uint32_t k = static_cast<std::uint32_t>(v.id_);
    if (k >= limit) return;
    while (k < limit && ix.entries[k].indent == v.col_ &&
           is_seq_kind(ix.entries[k].kind)) {
        fn(element(storage, k));
        k = ix.next[k];
    }
}

}  // namespace dftracer::utils::yaml
