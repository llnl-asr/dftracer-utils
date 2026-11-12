// SPDX-License-Identifier: MIT
#include <dftracer/utils/yaml/flow_parser.h>
#include <dftracer/utils/yaml/line_value.h>
#include <dftracer/utils/yaml/scalar.h>

#include <algorithm>
#include <cstring>

namespace dftracer::utils::yaml {

namespace {

/// The scalar text is in the arena of the index.
constexpr std::uint8_t ARENA_BIT = 0x80;
/// The value is a node of the tape: `id_` is its tape index.
constexpr std::uint8_t TAPE_BIT = 0x40;
constexpr std::uint8_t TAG_MASK = 0x03;

bool is_skipped(LineKind kind) {
    return kind == LineKind::Blank || kind == LineKind::Comment;
}

bool is_seq_kind(LineKind kind) {
    return kind == LineKind::SeqItem || kind == LineKind::SeqOpen ||
           kind == LineKind::SeqMap || kind == LineKind::SeqMapOpen ||
           kind == LineKind::NestItem || kind == LineKind::NestItemOpen ||
           kind == LineKind::NestMap || kind == LineKind::NestMapOpen;
}

bool is_nest_kind(LineKind kind) {
    return kind == LineKind::NestItem || kind == LineKind::NestItemOpen ||
           kind == LineKind::NestMap || kind == LineKind::NestMapOpen;
}

bool is_map_first_kind(LineKind kind) {
    return kind == LineKind::SeqMap || kind == LineKind::SeqMapOpen ||
           kind == LineKind::NestMap || kind == LineKind::NestMapOpen;
}

bool is_white(char c) { return c == ' ' || c == '\t'; }

std::uint64_t pack(std::uint32_t lo, std::uint32_t hi) {
    return lo | (static_cast<std::uint64_t>(hi) << 32);
}

LineTag tag_of(std::uint8_t tag) {
    return static_cast<LineTag>(tag & TAG_MASK);
}

// The table entry of a line's key, or of its value: a scalar of several lines
// is found through the first Cont line, any other by a search.
const EscapeEntry& escape_entry(const LineIndex& ix, std::uint32_t line,
                                bool is_value) {
    if (is_value && ix.entries[line + 1].kind == LineKind::Cont)
        return ix.escapes[ix.next[line + 1]];
    const std::uint32_t key = line * 2 + (is_value ? 1u : 0u);
    return *std::lower_bound(
        ix.escapes.begin(), ix.escapes.end(), key,
        [](const EscapeEntry& e, std::uint32_t k) { return e.key < k; });
}

dstd::StringView key_of(const LineIndex& ix, std::uint32_t line,
                        const LineParts& pt) {
    if ((pt.flags & line_flag::KEY_ESC) != 0) {
        const EscapeEntry& e = escape_entry(ix, line, false);
        return dstd::StringView(ix.arena.data() + e.off, e.len);
    }
    return dstd::StringView(ix.text + pt.key_s, pt.key_e - pt.key_s);
}

// The first line after `line` that is not a blank or a comment.
std::uint32_t content_after(const LineIndex& ix, std::uint32_t line) {
    std::uint32_t c = line + 1;
    while (is_skipped(ix.entries[c].kind)) ++c;
    return c;
}

constexpr std::uint8_t NOT_PLAIN =
    line_flag::KEY_QUOTED | line_flag::VAL_QUOTED | line_flag::FLOW |
    line_flag::COMMENT | line_flag::KEY_ESC | line_flag::VAL_ESC;

// The facts of a line. A line that holds no quote, comment or empty flow is
// read by a short scan, and any other by classify_line, which stage 1 ran on
// it already.
void parts_of(const LineIndex& ix, std::uint32_t line, LineParts& pt) {
    const LineEntry& e = ix.entries[line];
    const std::uint32_t end = ix.line_end(line);
    if ((e.flags & NOT_PLAIN) != 0 || e.kind == LineKind::Blank ||
        e.kind == LineKind::Comment || e.kind == LineKind::Root ||
        e.kind == LineKind::RootDoc || is_nest_kind(e.kind)) {
        classify_line(ix.text, e.start, end, pt);
        return;
    }
    const char* t = ix.text;
    std::uint32_t p = e.start + e.indent;
    const bool seq = is_seq_kind(e.kind);
    if (seq) {
        ++p;
        while (p < end && is_white(t[p])) ++p;
    }
    pt.kind = e.kind;
    pt.flags = e.flags & line_flag::VAL_EMPTY;
    pt.indent = e.indent;
    pt.content = p;
    if (e.kind == LineKind::SeqItem || e.kind == LineKind::SeqOpen) {
        pt.val_s = p;
        std::uint32_t ve = end;
        while (ve > p && is_white(t[ve - 1])) --ve;
        pt.val_e = ve;
        return;
    }
    std::uint32_t k = p;
    while (!(t[k] == ':' && (k + 1 == end || is_white(t[k + 1])))) ++k;
    std::uint32_t ke = k;
    while (ke > p && is_white(t[ke - 1])) --ke;
    pt.key_s = p;
    pt.key_e = ke;
    std::uint32_t v = k + 1;
    while (v < end && is_white(t[v])) ++v;
    pt.val_s = v;
    std::uint32_t ve = end;
    while (ve > v && is_white(t[ve - 1])) --ve;
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
    if (is_map_first_kind(first_kind)) {
        std::uint32_t c = k + 1;
        if (ix.entries[c].kind == LineKind::Cont)
            c = ix.escapes[ix.next[c]].end & ~ESCAPE_IN_TEXT;
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

Value root_scalar(const DocumentStorage* storage, std::uint32_t line);

}  // namespace

Value LineAccess::container(const DocumentStorage* storage, std::uint32_t first,
                            std::uint32_t limit) {
    const LineEntry& e = storage->lines.entries[first];
    if (e.kind == LineKind::Root || e.kind == LineKind::RootDoc)
        return root_scalar(storage, first);
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

Value LineAccess::make_tape(const DocumentStorage* storage, LineTag tag,
                            std::uint32_t index) {
    return Value(
        storage, index, 0,
        static_cast<std::uint8_t>(static_cast<std::uint8_t>(tag) | TAPE_BIT));
}

Value LineAccess::null(const DocumentStorage* storage) {
    return make(storage, LineTag::Null, 0, 0);
}

namespace {

LineTag tag_of_kind(TapeKind kind) {
    switch (kind) {
        case TapeKind::Map:
            return LineTag::Map;
        case TapeKind::Seq:
            return LineTag::Seq;
        case TapeKind::Scalar:
            return LineTag::Scalar;
        default:
            return LineTag::Null;
    }
}

// The value of the tape entry `index`.
Value tape_value(const DocumentStorage* storage, std::uint32_t index) {
    return LineAccess::make_tape(
        storage, tag_of_kind(storage->lines.tape[index].kind), index);
}

// The tape index after the node at `index`.
std::uint32_t tape_skip(const LineIndex& ix, std::uint32_t index) {
    const TapeEntry& entry = ix.tape[index];
    return entry.is_container() ? entry.pos : index + 1;
}

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
    const std::uint8_t flags = pt.flags | ix.entries[line].flags;
    if ((flags & line_flag::FLOW) != 0)
        return tape_value(storage, escape_entry(ix, line, true).off);
    if ((flags & line_flag::VAL_ESC) != 0) {
        const EscapeEntry& e = escape_entry(ix, line, true);
        if ((flags & line_flag::VAL_QUOTED) == 0 &&
            is_null_text(dstd::StringView((e.end & ESCAPE_IN_TEXT) == 0
                                              ? ix.arena.data() + e.off
                                              : ix.text + e.off,
                                          e.len)))
            return LineAccess::make(storage, LineTag::Null, 0, 0);
        return LineAccess::make(storage, LineTag::Scalar,
                                pack(e.off, e.off + e.len), 0,
                                (e.end & ESCAPE_IN_TEXT) == 0);
    }
    if ((pt.flags & line_flag::VAL_EMPTY) != 0)
        return LineAccess::make(storage, LineTag::Null, 0, 0);
    if ((pt.flags & line_flag::VAL_QUOTED) != 0)
        return LineAccess::make(storage, LineTag::Scalar,
                                pack(pt.val_s, pt.val_e), 0);
    if (is_null_text(dstd::StringView(ix.text + pt.val_s, pt.val_e - pt.val_s)))
        return LineAccess::make(storage, LineTag::Null, 0, 0);
    return LineAccess::make(storage, LineTag::Scalar, pack(pt.val_s, pt.val_e),
                            0);
}

Value root_scalar(const DocumentStorage* storage, std::uint32_t line) {
    LineParts pt;
    parts_of(storage->lines, line, pt);
    return inline_value(storage, line, pt);
}

Value member_value(const DocumentStorage* storage, std::uint32_t line,
                   const LineParts& pt) {
    const LineKind kind = storage->lines.entries[line].kind;
    if (kind == LineKind::MapOpen || kind == LineKind::SeqOpen ||
        kind == LineKind::SeqMapOpen || kind == LineKind::NestItemOpen ||
        kind == LineKind::NestMapOpen)
        return child_block(storage, line);
    return inline_value(storage, line, pt);
}

// The item at column `col` of the line `line`: for a line such as `- - a`, a
// dash that is followed by another one holds a sequence that starts there.
Value element_at(const DocumentStorage* storage, std::uint32_t line,
                 std::uint32_t col) {
    const LineIndex& ix = storage->lines;
    const LineEntry& e = ix.entries[line];
    if (e.kind == LineKind::SeqMap || e.kind == LineKind::SeqMapOpen) {
        std::uint32_t p = e.start + e.indent + 1;
        while (is_white(ix.text[p])) ++p;
        return LineAccess::make(storage, LineTag::Map,
                                pack(line, ix.next[line]), p - e.start);
    }
    LineParts pt;
    parts_of(ix, line, pt);
    if (is_nest_kind(e.kind)) {
        std::uint32_t p = pt.dash;
        for (std::uint32_t d = 0; d < pt.nest + 1; ++d) {
            if (p - e.start == col) {
                if (d == pt.nest) break;
                std::uint32_t q = p + 1;
                while (is_white(ix.text[q])) ++q;
                return LineAccess::make(storage, LineTag::Seq,
                                        pack(line, ix.next[line]), q - e.start);
            }
            ++p;
            while (is_white(ix.text[p])) ++p;
        }
        if (e.kind == LineKind::NestMap || e.kind == LineKind::NestMapOpen)
            return LineAccess::make(storage, LineTag::Map,
                                    pack(line, ix.next[line]),
                                    pt.content - e.start);
    }
    return member_value(storage, line, pt);
}

}  // namespace

Value LineAccess::element(const DocumentStorage* storage, std::uint32_t line) {
    return element_at(storage, line, storage->lines.entries[line].indent);
}

bool LineAccess::next_document(const DocumentStorage* storage,
                               std::uint32_t& pos, std::uint32_t& first,
                               std::uint32_t& limit, bool& is_empty) {
    const LineIndex& ix = storage->lines;
    std::uint32_t k = pos;
    while (k < ix.line_count) {
        const LineKind kind = ix.entries[k].kind;
        if (!is_skipped(kind) && kind != LineKind::DocEnd) break;
        ++k;
    }
    if (k >= ix.line_count) return false;
    const auto is_boundary = [&](std::uint32_t l) {
        const LineKind kind = ix.entries[l].kind;
        return kind == LineKind::DocStart || kind == LineKind::DocEnd ||
               kind == LineKind::RootDoc;
    };
    is_empty = false;
    first = k;
    if (ix.entries[k].kind == LineKind::DocStart) {
        std::uint32_t c = k + 1;
        while (c < ix.line_count && is_skipped(ix.entries[c].kind)) ++c;
        if (c >= ix.line_count || is_boundary(c)) {
            is_empty = true;
            first = limit = k;
            pos = c;
            return true;
        }
        first = c;
    }
    std::uint32_t l = first + 1;
    if (ix.has_markers) {
        while (l < ix.line_count && !is_boundary(l)) ++l;
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
    bool is_empty = false;
    if (!next_document(storage, pos, first, limit, is_empty) || is_empty)
        return null(storage);
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
    if ((v.tag_ & TAPE_BIT) != 0) return tape_text(ix, ix.tape[v.id_]);
    const char* base = (v.tag_ & ARENA_BIT) != 0 ? ix.arena.data() : ix.text;
    const auto s = static_cast<std::uint32_t>(v.id_);
    const auto e = static_cast<std::uint32_t>(v.id_ >> 32);
    return dstd::StringView(base + s, e - s);
}

dstd::Optional<Value> LineAccess::find(const Value& v, dstd::StringView key) {
    if (!is_object(v)) return dstd::nullopt;
    const DocumentStorage* storage = v.storage_;
    const LineIndex& ix = storage->lines;
    if ((v.tag_ & TAPE_BIT) != 0) {
        const auto map = static_cast<std::uint32_t>(v.id_);
        const std::uint32_t end = ix.tape[map].pos;
        for (std::uint32_t i = map + 1; i < end;) {
            if (tape_text(ix, ix.tape[i]) == key)
                return tape_value(storage, i + 1);
            i = tape_skip(ix, i + 1);
        }
        return dstd::nullopt;
    }
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
            if (after != ':' && !is_white(after)) return true;
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
                while (vs < end && is_white(t[vs])) ++vs;
                std::uint32_t ve = end;
                while (ve > vs && is_white(t[ve - 1])) --ve;
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
    if ((v.tag_ & TAPE_BIT) != 0) {
        const auto map = static_cast<std::uint32_t>(v.id_);
        const std::uint32_t end = ix.tape[map].pos;
        for (std::uint32_t i = map + 1; i < end;) {
            fn(tape_text(ix, ix.tape[i]), tape_value(storage, i + 1));
            i = tape_skip(ix, i + 1);
        }
        return;
    }
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
    if ((v.tag_ & TAPE_BIT) != 0) {
        const auto seq = static_cast<std::uint32_t>(v.id_);
        const std::uint32_t end = ix.tape[seq].pos;
        for (std::uint32_t i = seq + 1; i < end; i = tape_skip(ix, i))
            fn(tape_value(storage, i));
        return;
    }
    const auto limit = static_cast<std::uint32_t>(v.id_ >> 32);
    std::uint32_t k = static_cast<std::uint32_t>(v.id_);
    if (k >= limit) return;
    fn(element_at(storage, k, v.col_));
    if (ix.entries[k].indent == v.col_) {
        k = ix.next[k];
    } else {
        // The first item starts inside the line, after a dash: skip what is
        // under it.
        std::uint32_t c = k + 1;
        if (ix.entries[c].kind == LineKind::Cont)
            c = ix.escapes[ix.next[c]].end & ~ESCAPE_IN_TEXT;
        while (c < limit && is_skipped(ix.entries[c].kind)) ++c;
        while (c < limit && ix.entries[c].indent > v.col_) c = ix.next[c];
        k = c;
    }
    while (k < limit && ix.entries[k].indent == v.col_ &&
           is_seq_kind(ix.entries[k].kind)) {
        fn(element(storage, k));
        k = ix.next[k];
    }
}

}  // namespace dftracer::utils::yaml
