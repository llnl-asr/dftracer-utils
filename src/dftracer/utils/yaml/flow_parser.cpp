// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/yaml/flow_parser.h>
#include <dftracer/utils/yaml/line_scalar.h>
#include <dftracer/utils/yaml/scalar.h>

#include <cstring>

namespace dftracer::utils::yaml {

namespace {

constexpr std::uint32_t NONE = 0xFFFFFFFFu;

bool is_white(char c) { return c == ' ' || c == '\t'; }

bool is_flow_indicator(char c) {
    return c == ',' || c == '[' || c == ']' || c == '{' || c == '}';
}

}  // namespace

bool FlowParser::fail(std::uint32_t offset, const char* reason) {
    return detail::fail_at(ix_, offset, reason);
}

// Whether a `---` or `...` marker starts at `at`, which is a line start.
bool FlowParser::is_marker(std::uint32_t at) const {
    if (at + 3 > size_ || (t_[at] != '-' && t_[at] != '.') ||
        t_[at + 1] != t_[at] || t_[at + 2] != t_[at])
        return false;
    return at + 3 == size_ || is_white(t_[at + 3]) || t_[at + 3] == '\n' ||
           t_[at + 3] == '\r';
}

// Whether the byte at `at` ends a `:` that makes a key a key: the end of the
// text, a blank, a line break or a flow indicator.
bool FlowParser::separates(std::uint32_t at) const {
    if (at >= size_) return true;
    const char c = t_[at];
    return is_white(c) || c == '\n' || c == '\r' || is_flow_indicator(c);
}

// The line that starts at `at` after a line break: it holds no marker, and a
// token on it is indented enough.
bool FlowParser::check_line_start(std::uint32_t at) {
    if (is_marker(at))
        return fail(at, "a document marker inside a flow collection");
    std::uint32_t i = at;
    while (i < size_ && t_[i] == ' ') ++i;
    const std::uint32_t indent = i - at;
    while (i < size_ && is_white(t_[i])) ++i;
    if (i >= size_ || t_[i] == '\n' || t_[i] == '\r' || t_[i] == '#')
        return true;
    if (indent < min_indent_)
        return fail(i, "a flow collection is not indented enough");
    return true;
}

// Skips blanks, line breaks and comments.
bool FlowParser::skip(bool& crossed) {
    crossed = false;
    bool is_after_white = false;
    while (pos_ < size_) {
        const char c = t_[pos_];
        if (c == ' ' || c == '\t' || c == '\r') {
            is_after_white = true;
            ++pos_;
        } else if (c == '\n') {
            ++pos_;
            crossed = true;
            is_after_white = true;
            if (!check_line_start(pos_)) return false;
        } else if (c == '#' && is_after_white) {
            const void* nl = std::memchr(t_ + pos_, '\n', size_ - pos_);
            pos_ = nl == nullptr ? size_
                                 : static_cast<std::uint32_t>(
                                       static_cast<const char*>(nl) - t_);
        } else {
            break;
        }
    }
    return true;
}

void FlowParser::push_null() {
    ix_.tape.push_back(TapeEntry{0, 0, TapeKind::Null, 0});
}

std::uint32_t FlowParser::push_scalar(std::uint32_t pos, std::uint32_t size,
                                      std::uint8_t flags, TapeKind kind) {
    const auto index = static_cast<std::uint32_t>(ix_.tape.size());
    ix_.tape.push_back(TapeEntry{pos, size, kind, flags});
    return index;
}

bool FlowParser::parse(std::uint32_t start, std::uint32_t min_indent,
                       std::uint32_t& end, std::uint32_t& root) {
    t_ = ix_.text;
    size_ = ix_.size;
    pos_ = start;
    min_indent_ = min_indent;
    root = static_cast<std::uint32_t>(ix_.tape.size());
    Node out;
    if (!node(0, out)) return false;
    end = pos_;
    return true;
}

bool FlowParser::node(std::size_t depth, Node& out) {
    const char c = t_[pos_];
    switch (c) {
        case '[':
            return seq(depth, out);
        case '{':
            return map(depth, out);
        case '"':
        case '\'':
            return quoted(out);
        case '&':
            return fail(pos_, "anchors are not supported");
        case '*':
            return fail(pos_, "aliases are not supported");
        case '!':
            return fail(pos_, "tags are not supported");
        case '|':
        case '>':
            return fail(pos_, "a block scalar cannot be in a flow collection");
        case '%':
            return fail(pos_, "a plain scalar cannot start with '%'");
        case '@':
            return fail(pos_,
                        "'@' is reserved and cannot start a plain scalar");
        case '`':
            return fail(pos_,
                        "'`' is reserved and cannot start a plain scalar");
        case ',':
        case ']':
        case '}':
            return fail(pos_, "a flow collection has an entry with no value");
        case '#':
            return fail(pos_, "a comment needs a blank before it");
        default:
            return plain(out);
    }
}

bool FlowParser::seq(std::size_t depth, Node& out) {
    if (depth >= MAX_DEPTH)
        return fail(pos_,
                    "flow collections nested deeper than 128 levels "
                    "are not supported");
    const std::uint32_t open = pos_;
    ++pos_;
    const auto begin = static_cast<std::uint32_t>(ix_.tape.size());
    ix_.tape.push_back(TapeEntry{0, 0, TapeKind::Seq, 0});
    bool crossed = false;
    for (;;) {
        if (!skip(crossed)) return false;
        if (pos_ >= size_) return fail(open, "unterminated flow sequence");
        char c = t_[pos_];
        if (c == ']') {
            ++pos_;
            break;
        }
        if (c == ',')
            return fail(pos_, "a flow sequence has an entry with no value");
        if (!seq_entry(depth)) return false;
        if (!skip(crossed)) return false;
        if (pos_ >= size_) return fail(open, "unterminated flow sequence");
        c = t_[pos_];
        if (c == ',') {
            ++pos_;
            continue;
        }
        if (c == ']') {
            ++pos_;
            break;
        }
        return fail(pos_, "expected ',' or ']' in a flow sequence");
    }
    ix_.tape[begin].pos = static_cast<std::uint32_t>(ix_.tape.size());
    out.index = begin;
    out.is_collection = true;
    out.is_json_like = true;
    return true;
}

// An entry of a sequence: a node, or `key: value` that makes a map of one pair.
bool FlowParser::seq_entry(std::size_t depth) {
    const char c = t_[pos_];
    if (c == '?' && separates(pos_ + 1) && pos_ + 1 < size_ &&
        t_[pos_ + 1] != ',' && t_[pos_ + 1] != ']' && t_[pos_ + 1] != '[' &&
        t_[pos_ + 1] != '{' && t_[pos_ + 1] != '}')
        return fail(pos_, "complex keys are not supported");
    if (c == ':' && separates(pos_ + 1))
        return fail(pos_, "a null key is not supported");
    const std::uint32_t key_at = pos_;
    const auto mark = static_cast<std::uint32_t>(ix_.tape.size());
    Node key;
    if (!node(depth + 1, key)) return false;
    std::uint32_t colon = pos_;
    while (colon < size_ && is_white(t_[colon])) ++colon;
    if (colon >= size_ || t_[colon] != ':' ||
        !(key.is_json_like || separates(colon + 1)))
        return true;
    pos_ = colon;
    if (key.is_collection) return fail(key_at, "a collection cannot be a key");
    if (key.is_multi_line)
        return fail(key_at, "an implicit key must be on one line");
    if (key.is_null) return fail(key_at, "a null key is not supported");
    // The pair is a map of its own: its begin goes in front of the key.
    ix_.tape.push_back(ix_.tape[mark]);
    ix_.tape[mark] = TapeEntry{0, 0, TapeKind::Map, 0};
    ix_.tape[mark + 1].flags |= tape_flag::KEY;
    ++pos_;
    bool crossed = false;
    if (!skip(crossed)) return false;
    if (pos_ >= size_ || t_[pos_] == ',' || t_[pos_] == ']') {
        push_null();
    } else {
        Node value;
        if (!node(depth + 1, value)) return false;
    }
    ix_.tape[mark].pos = static_cast<std::uint32_t>(ix_.tape.size());
    return true;
}

bool FlowParser::map(std::size_t depth, Node& out) {
    if (depth >= MAX_DEPTH)
        return fail(pos_,
                    "flow collections nested deeper than 128 levels "
                    "are not supported");
    const std::uint32_t open = pos_;
    ++pos_;
    const auto begin = static_cast<std::uint32_t>(ix_.tape.size());
    ix_.tape.push_back(TapeEntry{0, 0, TapeKind::Map, 0});
    KeySet::Level& level = levels_[depth];
    keys_.open(level);
    bool crossed = false;
    for (;;) {
        if (!skip(crossed)) return false;
        if (pos_ >= size_) return fail(open, "unterminated flow mapping");
        char c = t_[pos_];
        if (c == '}') {
            ++pos_;
            break;
        }
        if (c == ',')
            return fail(pos_, "a flow mapping has an entry with no key");
        if (!map_entry(depth, level)) return false;
        if (!skip(crossed)) return false;
        if (pos_ >= size_) return fail(open, "unterminated flow mapping");
        c = t_[pos_];
        if (c == ',') {
            ++pos_;
            continue;
        }
        if (c == '}') {
            ++pos_;
            break;
        }
        return fail(pos_, "expected ',' or '}' in a flow mapping");
    }
    keys_.close(level, depth);
    ix_.tape[begin].pos = static_cast<std::uint32_t>(ix_.tape.size());
    out.index = begin;
    out.is_collection = true;
    out.is_json_like = true;
    return true;
}

bool FlowParser::map_entry(std::size_t depth, KeySet::Level& level) {
    const char c = t_[pos_];
    if (c == '?' && pos_ + 1 < size_ &&
        (is_white(t_[pos_ + 1]) || t_[pos_ + 1] == '\n' ||
         t_[pos_ + 1] == '\r'))
        return fail(pos_, "complex keys are not supported");
    if (c == ':' && separates(pos_ + 1))
        return fail(pos_, "a null key is not supported");
    const std::uint32_t key_at = pos_;
    Node key;
    if (!node(depth + 1, key)) return false;
    if (key.is_collection) return fail(key_at, "a collection cannot be a key");
    if (key.is_null) return fail(key_at, "a null key is not supported");
    ix_.tape[key.index].flags |= tape_flag::KEY;
    const dstd::StringView text = tape_text(ix_, ix_.tape[key.index]);
    const bool is_in_text = text.data() >= t_ && text.data() < t_ + size_;
    const bool is_new = keys_.add(
        level, depth, key.index, text, is_in_text ? t_ + size_ : text.data(),
        [&](std::uint32_t id, dstd::StringView other) {
            return tape_text(ix_, ix_.tape[id]) == other;
        });
    if (!is_new)
        return fail(key_at, dstd::fmt<"duplicate key '{}'">(text).c_str());
    bool crossed = false;
    if (!skip(crossed)) return false;
    if (pos_ < size_ && t_[pos_] == ':' &&
        (key.is_json_like || separates(pos_ + 1))) {
        ++pos_;
        if (!skip(crossed)) return false;
        if (pos_ >= size_ || t_[pos_] == ',' || t_[pos_] == '}') {
            push_null();
        } else {
            Node value;
            if (!node(depth + 1, value)) return false;
        }
    } else {
        push_null();
    }
    return true;
}

bool FlowParser::quoted(Node& out) {
    const std::uint32_t open = pos_;
    const char quote = t_[open];
    const bool is_double = quote == '"';
    std::uint32_t i = open + 1;
    bool has_escape = false;
    bool is_multi_line = false;
    std::uint32_t bad_at = NONE;
    const char* bad_reason = nullptr;
    auto note_line = [&](std::uint32_t at) {
        if (bad_at != NONE) return;
        if (is_marker(at)) {
            bad_at = at;
            bad_reason = "a document marker inside a quoted scalar";
            return;
        }
        std::uint32_t k = at;
        while (k < size_ && t_[k] == ' ') ++k;
        const std::uint32_t indent = k - at;
        while (k < size_ && is_white(t_[k])) ++k;
        if (k < size_ && t_[k] != '\n' && t_[k] != '\r' &&
            indent < min_indent_) {
            bad_at = k;
            bad_reason = "a quoted scalar is not indented enough";
        }
    };
    for (;;) {
        if (i >= size_) return fail(open, "unterminated quoted scalar");
        const char c = t_[i];
        if (c == '\\' && is_double) {
            has_escape = true;
            ++i;
            if (i < size_ && t_[i] == '\n') {
                is_multi_line = true;
                note_line(i + 1);
            }
            ++i;
            continue;
        }
        if (c == quote) {
            if (!is_double && i + 1 < size_ && t_[i + 1] == quote) {
                has_escape = true;
                i += 2;
                continue;
            }
            break;
        }
        if (c == '\n') {
            is_multi_line = true;
            note_line(i + 1);
        }
        ++i;
    }
    if (bad_at != NONE) return fail(bad_at, bad_reason);
    std::uint32_t pos = open + 1;
    std::uint32_t size = i - (open + 1);
    std::uint8_t flags = tape_flag::QUOTED;
    if (has_escape || is_multi_line) {
        detail::EscapeProblem problem;
        detail::ScalarText text;
        if (detail::decode_quoted(ix_, open + 1, i, is_double, problem, text) !=
            detail::Decoded::Ok)
            return fail(problem.at, detail::escape_reason(problem).c_str());
        pos = text.off;
        size = text.len;
        flags |= tape_flag::ARENA;
    }
    out.index = push_scalar(pos, size, flags, TapeKind::Scalar);
    out.is_collection = false;
    out.is_json_like = true;
    out.is_multi_line = is_multi_line;
    out.is_null = false;
    pos_ = i + 1;
    return true;
}

bool FlowParser::plain(Node& out) {
    const std::uint32_t start = pos_;
    const char first = t_[start];
    if (first == '-' || first == '?' || first == ':') {
        if (separates(start + 1)) {
            if (first == '-')
                return fail(start,
                            "a sequence entry is not allowed in a flow "
                            "collection");
            if (first == '?')
                return fail(start, "complex keys are not supported");
            return fail(start, "unexpected ':' in a flow collection");
        }
    }
    std::uint32_t i = start;
    std::uint32_t last = start;
    bool is_multi_line = false;
    while (i < size_) {
        const char c = t_[i];
        if (is_flow_indicator(c)) break;
        if (c == ':') {
            if (separates(i + 1)) break;
            ++i;
            last = i;
            continue;
        }
        if (c == '#' && i > start && is_white(t_[i - 1])) break;
        if (c == '\n') {
            std::uint32_t j = i + 1;
            std::uint32_t k = j;
            bool is_ended = false;
            for (;;) {
                if (is_marker(j)) {
                    is_ended = true;
                    break;
                }
                k = j;
                while (k < size_ && t_[k] == ' ') ++k;
                const std::uint32_t indent = k - j;
                while (k < size_ && is_white(t_[k])) ++k;
                if (k >= size_) {
                    is_ended = true;
                    break;
                }
                if (t_[k] == '\n') {
                    j = k + 1;
                    continue;
                }
                if (t_[k] == '\r' && k + 1 < size_ && t_[k + 1] == '\n') {
                    j = k + 2;
                    continue;
                }
                is_ended = indent < min_indent_ || t_[k] == '#' ||
                           is_flow_indicator(t_[k]) ||
                           (t_[k] == ':' && separates(k + 1));
                break;
            }
            if (is_ended) break;
            is_multi_line = true;
            i = k;
            continue;
        }
        ++i;
        if (c != ' ' && c != '\t' && c != '\r') last = i;
    }
    pos_ = last;
    out.is_collection = false;
    out.is_json_like = false;
    out.is_multi_line = is_multi_line;
    if (!is_multi_line) {
        const dstd::StringView text(t_ + start, last - start);
        out.is_null = is_null_text(text);
        out.index =
            push_scalar(start, last - start, 0,
                        out.is_null ? TapeKind::Null : TapeKind::Scalar);
        return true;
    }
    detail::ScalarText text;
    detail::decode_plain(ix_, start, last, text);
    out.is_null = false;
    out.index =
        push_scalar(text.off, text.len, tape_flag::ARENA, TapeKind::Scalar);
    return true;
}

}  // namespace dftracer::utils::yaml
