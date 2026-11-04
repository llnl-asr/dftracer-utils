// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_arena.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/json/value_access.h>
#include <simdjson.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace dftracer::utils::json {

namespace detail {

struct DocumentState {
    simdjson::ondemand::parser parser;
    dstd::String buffer;
    std::size_t length = 0;
    simdjson::ondemand::document doc;
    dstd::StringArena strings;
    bool broken = false;
    // A value below the root has read since the root last did: the root's
    // iterator no longer sits where its next read expects.
    bool disturbed = false;
    // Callbacks of for_each_member and for_each_element that are running.
    int walking = 0;
    dstd::UniquePtr<simdjson::ondemand::parser> child_parser;
    simdjson::ondemand::document child_doc;

    const char* end() const { return buffer.data() + length; }
};

}  // namespace detail

namespace {

using OnDemand = simdjson::ondemand::value;
using Access = detail::ValueAccess;

constexpr std::uint8_t FRESH = 0;
constexpr std::uint8_t PROBED = 1;
constexpr std::uint8_t SPENT = 2;

// A read error other than "no such field", "wrong type" or "past the end"
// leaves simdjson's iterator unusable: the line is marked broken and every
// later read of it comes back empty.
bool note(detail::DocumentState& st, simdjson::error_code code) {
    if (code != simdjson::SUCCESS && code != simdjson::NO_SUCH_FIELD &&
        code != simdjson::INCORRECT_TYPE &&
        code != simdjson::INDEX_OUT_OF_BOUNDS)
        st.broken = true;
    return code == simdjson::SUCCESS;
}

struct Walk {
    detail::DocumentState& st;
    explicit Walk(detail::DocumentState& s) : st(s) { ++st.walking; }
    ~Walk() { --st.walking; }
};

// A skip over a malformed part can abandon the iterator without an error
// reaching a read, and any use of the dead iterator crashes.
bool usable(detail::DocumentState& st) {
    if (st.broken) return false;
    if (!st.doc.is_alive()) st.broken = true;
    return !st.broken;
}

simdjson::error_code open_document(detail::DocumentState& st,
                                   simdjson::ondemand::value& root) {
    auto parsed =
        st.parser.iterate(st.buffer.data(), st.length, st.buffer.size());
    if (parsed.error()) return parsed.error();
    st.doc = std::move(parsed.value_unsafe());
    return st.doc.get_value().get(root);
}

dstd::Unexpected<dstd::Error> parse_error(simdjson::error_code code) {
    return common::make_error(
        common::ErrorCode::Parse,
        dstd::String("invalid JSON: ") + simdjson::error_message(code));
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Past the closing quote of the string whose opening quote is at `q`.
const char* skip_string(const char* q, const char* end) {
    ++q;
    while (q < end && *q != '"') {
        if (*q == '\\') ++q;
        ++q;
    }
    return q < end ? q + 1 : end;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool read_hex4(const char* p, const char* end, unsigned& out) {
    if (end - p < 4) return false;
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) {
        const int d = hex_digit(p[i]);
        if (d < 0) return false;
        v = (v << 4) | static_cast<unsigned>(d);
    }
    out = v;
    return true;
}

char* put_utf8(char* out, unsigned cp) {
    if (cp < 0x80) {
        *out++ = static_cast<char>(cp);
    } else if (cp < 0x800) {
        *out++ = static_cast<char>(0xC0 | (cp >> 6));
        *out++ = static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *out++ = static_cast<char>(0xE0 | (cp >> 12));
        *out++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out++ = static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        *out++ = static_cast<char>(0xF0 | (cp >> 18));
        *out++ = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        *out++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out++ = static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

// The decoded text is never longer than the escaped text, so `dst` needs
// text.size() bytes.
bool unescape(dstd::StringView text, char* dst, std::size_t& length) {
    const char* p = text.data();
    const char* const end = p + text.size();
    char* out = dst;
    while (p < end) {
        if (*p != '\\') {
            *out++ = *p++;
            continue;
        }
        if (++p == end) return false;
        switch (*p++) {
            case '"':
                *out++ = '"';
                break;
            case '\\':
                *out++ = '\\';
                break;
            case '/':
                *out++ = '/';
                break;
            case 'b':
                *out++ = '\b';
                break;
            case 'f':
                *out++ = '\f';
                break;
            case 'n':
                *out++ = '\n';
                break;
            case 'r':
                *out++ = '\r';
                break;
            case 't':
                *out++ = '\t';
                break;
            case 'u': {
                unsigned cp;
                if (!read_hex4(p, end, cp)) return false;
                p += 4;
                if (cp >= 0xD800 && cp < 0xDC00) {
                    unsigned low;
                    if (end - p < 6 || p[0] != '\\' || p[1] != 'u' ||
                        !read_hex4(p + 2, end, low) || low < 0xDC00 ||
                        low > 0xDFFF)
                        return false;
                    p += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return false;
                }
                out = put_utf8(out, cp);
                break;
            }
            default:
                return false;
        }
    }
    length = static_cast<std::size_t>(out - dst);
    return true;
}

}  // namespace

namespace detail {

dstd::StringView value_extent(const char* start, const char* end) {
    const char* q = start;
    if (q >= end) return {};
    if (*q == '{' || *q == '[') {
        int depth = 0;
        for (; q < end; ++q) {
            const char c = *q;
            if (c == '"') {
                q = skip_string(q, end) - 1;
            } else if (c == '{' || c == '[') {
                ++depth;
            } else if (c == '}' || c == ']') {
                if (--depth == 0) {
                    ++q;
                    break;
                }
            }
        }
    } else if (*q == '"') {
        q = skip_string(q, end);
    } else {
        while (q < end && !is_space(*q) && *q != ',' && *q != '}' && *q != ']')
            ++q;
    }
    q = std::min(q, end);
    return {start, static_cast<std::size_t>(q - start)};
}

dstd::Optional<dstd::StringView> decode_string(dstd::StringView text,
                                               dstd::StringArena& arena) {
    if (!std::memchr(text.data(), '\\', text.size())) return text;
    // Reserve the whole escaped length: the decoded text is never longer.
    const dstd::StringView room = arena.push(text.data(), text.size());
    char* dst = const_cast<char*>(room.data());
    std::size_t length;
    // The copy in the arena is the source, so decode over it in place: a
    // decoded byte is never written ahead of the byte being read.
    if (!unescape(room, dst, length)) return text;
    return dstd::StringView(dst, length);
}

dstd::Optional<dstd::StringView> read_string(dstd::StringView token,
                                             dstd::StringArena& arena) {
    if (token.size() < 2 || token.front() != '"' || token.back() != '"')
        return dstd::nullopt;
    return decode_string(token.substr(1, token.size() - 2), arena);
}

dstd::Optional<dstd::StringView> read_key(
    simdjson::ondemand::raw_json_string key, const char* end,
    dstd::StringArena& arena) {
    const char* begin = key.raw();
    const char* q = begin;
    while (q < end && *q != '"') {
        if (*q == '\\') ++q;
        ++q;
    }
    if (q >= end) return dstd::nullopt;
    return decode_string(
        dstd::StringView(begin, static_cast<std::size_t>(q - begin)), arena);
}

dstd::Optional<double> refused_number(dstd::StringView token) {
    constexpr long EXPONENT_CAP = 100000000;
    const char* p = token.data();
    const char* const end = p + token.size();
    const bool negative = p < end && *p == '-';
    if (negative) ++p;
    const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    if (p == end || !is_digit(*p)) return dstd::nullopt;
    const char* const int_begin = p;
    if (*p == '0') {
        ++p;
    } else {
        while (p < end && is_digit(*p)) ++p;
    }
    const char* const int_end = p;
    const char* frac_begin = p;
    const char* frac_end = p;
    if (p < end && *p == '.') {
        frac_begin = ++p;
        while (p < end && is_digit(*p)) ++p;
        frac_end = p;
        if (frac_end == frac_begin) return dstd::nullopt;
    }
    long exponent = 0;
    if (p < end && (*p == 'e' || *p == 'E')) {
        ++p;
        bool exponent_negative = false;
        if (p < end && (*p == '+' || *p == '-'))
            exponent_negative = *p++ == '-';
        const char* const digits = p;
        for (; p < end && is_digit(*p); ++p)
            if (exponent < EXPONENT_CAP) exponent = exponent * 10 + (*p - '0');
        if (p == digits) return dstd::nullopt;
        if (exponent_negative) exponent = -exponent;
    }
    if (p != end) return dstd::nullopt;
    long leading_zeros = 0;
    bool nonzero = false;
    for (const char* d = int_begin; d < int_end && !nonzero; ++d) {
        if (*d == '0')
            ++leading_zeros;
        else
            nonzero = true;
    }
    for (const char* d = frac_begin; d < frac_end && !nonzero; ++d) {
        if (*d == '0')
            ++leading_zeros;
        else
            nonzero = true;
    }
    const double sign = negative ? -1.0 : 1.0;
    if (!nonzero) return sign * 0.0;
    // The decimal exponent of the first nonzero digit.
    const long magnitude =
        static_cast<long>(int_end - int_begin) - 1 - leading_zeros + exponent;
    if (magnitude >= 308) return sign * std::numeric_limits<double>::infinity();
    if (magnitude < -330) return sign * 0.0;
    return dstd::nullopt;
}

dstd::Optional<dstd::StringView> raw_string(simdjson::ondemand::value& value) {
    const dstd::StringView token = trim_token(value.raw_json_token());
    if (token.size() < 2 || token.front() != '"' || token.back() != '"')
        return dstd::nullopt;
    return token.substr(1, token.size() - 2);
}

dstd::StringView raw_key(const char* begin) {
    const char* q = begin;
    while (*q != '"') {
        if (*q == '\\') ++q;
        ++q;
    }
    return dstd::StringView(begin, static_cast<std::size_t>(q - begin));
}

namespace {

// The length of the valid UTF-8 sequence that starts at `p`, or 0.
std::size_t utf8_length(const unsigned char* p, std::size_t left) {
    const unsigned char c = p[0];
    std::size_t need;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) {
        need = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
        need = 3;
        if (c == 0xE0) low = 0xA0;
        if (c == 0xED) high = 0x9F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        need = 4;
        if (c == 0xF0) low = 0x90;
        if (c == 0xF4) high = 0x8F;
    } else {
        return 0;
    }
    if (left < need || p[1] < low || p[1] > high) return 0;
    for (std::size_t i = 2; i < need; ++i)
        if (p[i] < 0x80 || p[i] > 0xBF) return 0;
    return need;
}

}  // namespace

void repair_line(char* data, std::size_t size) {
    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = 0; i < size; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        if (!in_string) {
            in_string = c == '"';
            continue;
        }
        if (!escaped) {
            if (c == '"') {
                in_string = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
        }
        const bool after_backslash = escaped;
        escaped = false;
        if (c < 0x20) {
            data[i] = '?';
            if (after_backslash) data[i - 1] = '?';
        } else if (c >= 0x80) {
            const std::size_t length = utf8_length(
                reinterpret_cast<const unsigned char*>(data) + i, size - i);
            if (length == 0) {
                data[i] = '?';
                if (after_backslash) data[i - 1] = '?';
            } else {
                i += length - 1;
            }
        }
    }
}

bool is_integer_number(dstd::StringView token) {
    std::size_t i = 0;
    if (i < token.size() && token[i] == '-') ++i;
    if (i == token.size()) return false;
    if (token[i] == '0') return i + 1 == token.size();
    if (token[i] < '1' || token[i] > '9') return false;
    for (++i; i < token.size(); ++i)
        if (token[i] < '0' || token[i] > '9') return false;
    return true;
}

Value ValueAccess::make(DocumentState* owner, OnDemand v, bool root) {
    static_assert(
        sizeof(OnDemand) <= Value::STORAGE_SIZE && alignof(OnDemand) <= 8,
        "Value::storage_ must hold an on-demand value");
    static_assert(std::is_trivially_copyable_v<OnDemand> &&
                      std::is_trivially_destructible_v<OnDemand>,
                  "Value moves its on-demand value as bytes");
    Value out;
    ::new (static_cast<void*>(out.storage_)) OnDemand(v);
    out.owner_ = owner;
    out.root_ = root;
    out.start_ = v.raw_json_token().data();
    return out;
}

OnDemand& ValueAccess::inner(Value& v) {
    return *std::launder(reinterpret_cast<OnDemand*>(v.storage_));
}

const OnDemand& ValueAccess::inner(const Value& v) {
    return *std::launder(reinterpret_cast<const OnDemand*>(v.storage_));
}

bool ValueAccess::broken(const Value& v) {
    return v.owner_ && v.owner_->broken;
}

void ValueAccess::mark_broken(const Value& v) {
    if (v.owner_) v.owner_->broken = true;
}

bool ValueAccess::event(const Value& v, EventView& out) {
    if (!v.owner_ || !v.start_ || !usable(*v.owner_)) return false;
    DocumentState& st = *v.owner_;
    out.strings = &st.strings;
    if (v.root_) {
        if (st.walking > 0) return false;
        v.state_ = SPENT;
        out.doc = &st.doc;
        out.text = dstd::StringView(st.buffer.data(), st.length);
        return true;
    }
    const dstd::StringView text = value_extent(v.start_, st.end());
    if (!st.child_parser)
        st.child_parser = dstd::make_unique<simdjson::ondemand::parser>();
    const char* const limit = st.buffer.data() + st.buffer.size();
    if (st.child_parser
            ->iterate(text.data(), text.size(),
                      static_cast<std::size_t>(limit - text.data()))
            .get(st.child_doc))
        return false;
    out.doc = &st.child_doc;
    out.text = text;
    return true;
}

bool ValueAccess::begin_object(Value& v, ObjectWalk& out) {
    if (!v.owner_ || !v.start_ || *v.start_ != '{' || !v.fresh_for_read())
        return false;
    if (!note(*v.owner_, inner(v).get_object().get(out.object))) return false;
    v.state_ = SPENT;
    out.end = v.owner_->end();
    out.strings = &v.owner_->strings;
    return true;
}

bool ValueAccess::begin_value(Value& v, ValueWalk& out) {
    if (!v.owner_ || !v.start_ || !v.fresh_for_read()) return false;
    out.value = inner(v);
    v.state_ = SPENT;
    out.end = v.owner_->end();
    out.strings = &v.owner_->strings;
    return true;
}

bool ValueAccess::end_object(const Value& v) {
    return v.owner_ && usable(*v.owner_);
}

void ValueAccess::rewind(Value& v) {
    if (v.root_ && (v.state_ != FRESH || v.owner_->disturbed)) v.restart_root();
}

}  // namespace detail

Document::Document() : state_(dstd::make_unique<detail::DocumentState>()) {}

Document::~Document() = default;
Document::Document(Document&&) noexcept = default;
Document& Document::operator=(Document&&) noexcept = default;

dstd::Result<Value> Document::parse(dstd::StringView line) {
    detail::DocumentState& st = *state_;
    st.buffer.assign(line.data() ? line.data() : "", line.size());
    st.buffer.resize(line.size() + simdjson::SIMDJSON_PADDING);
    st.length = line.size();
    st.strings.clear();
    st.broken = false;
    st.disturbed = false;
    OnDemand root;
    simdjson::error_code code = open_document(st, root);
    if (detail::is_repairable(code)) {
        detail::repair_line(st.buffer.data(), st.length);
        code = open_document(st, root);
    }
    if (code) return parse_error(code);
    return Access::make(&st, root, true);
}

Value::Type Value::type() {
    switch (*start_) {
        case '{':
            return Type::Object;
        case '[':
            return Type::Array;
        case '"':
            return Type::String;
        case 't':
        case 'f':
            return Type::Bool;
        case 'n':
            return Type::Null;
        default:
            break;
    }
    const dstd::StringView token = detail::value_extent(start_, owner_->end());
    if (token.empty()) return Type::Null;
    bool integer = true;
    for (char c : token) {
        if (c == '.' || c == 'e' || c == 'E' || c == '+') {
            integer = false;
        } else if (c != '-' && (c < '0' || c > '9')) {
            return Type::Null;
        }
    }
    if (!integer) return Type::Double;
    std::int64_t ignored;
    const auto r =
        dstd::from_chars(token.data(), token.data() + token.size(), ignored);
    return r.ec == std::errc() && r.ptr == token.data() + token.size()
               ? Type::Int
               : Type::Double;
}

dstd::StringView Value::raw() {
    return detail::value_extent(start_, owner_->end());
}

dstd::Optional<Value> Value::find(dstd::StringView key) {
    if (*start_ != '{' || !usable(*owner_)) return dstd::nullopt;
    if (!root_) {
        if (state_ == SPENT) return dstd::nullopt;
        owner_->disturbed = true;
    } else if (state_ == SPENT || owner_->disturbed) {
        if (!restart_root()) return dstd::nullopt;
    }
    OnDemand child;
    const bool found = note(
        *owner_, Access::inner(*this).find_field_unordered(key).get(child));
    state_ = PROBED;
    if (!found) return dstd::nullopt;
    return Access::make(owner_, child, false);
}

void Value::for_each_member(
    dstd::FunctionRef<void(dstd::StringView, Value)> fn) {
    if (*start_ != '{' || !fresh_for_read()) return;
    simdjson::ondemand::object object;
    if (!note(*owner_, Access::inner(*this).get_object().get(object))) return;
    state_ = SPENT;
    const Walk walk(*owner_);
    for (auto field_result : object) {
        simdjson::ondemand::field field;
        if (!note(*owner_, std::move(field_result).get(field))) break;
        const auto name =
            detail::read_key(field.key(), owner_->end(), owner_->strings);
        const OnDemand member = field.value();
        if (!name) continue;
        fn(*name, Access::make(owner_, member, false));
    }
}

void Value::for_each_element(dstd::FunctionRef<void(Value)> fn) {
    if (*start_ != '[' || !fresh_for_read()) return;
    simdjson::ondemand::array array;
    if (!note(*owner_, Access::inner(*this).get_array().get(array))) return;
    state_ = SPENT;
    const Walk walk(*owner_);
    for (auto element : array) {
        OnDemand item;
        if (!note(*owner_, element.get(item))) break;
        fn(Access::make(owner_, item, false));
    }
}

dstd::Optional<std::int64_t> Value::as_int() {
    const char c = *start_;
    if ((c != '-' && (c < '0' || c > '9')) || !fresh_for_read())
        return dstd::nullopt;
    // A number outside int64 or in double form is not an int; asking the
    // parser would only raise an error.
    const dstd::StringView token = raw();
    std::int64_t out;
    const auto parsed =
        dstd::from_chars(token.data(), token.data() + token.size(), out);
    if (parsed.ec != std::errc() || parsed.ptr != token.data() + token.size())
        return dstd::nullopt;
    if (Access::inner(*this).get_int64().get(out)) {
        owner_->broken = true;
        return dstd::nullopt;
    }
    state_ = SPENT;
    return out;
}

dstd::Optional<double> Value::as_double() {
    const char c = *start_;
    if ((c != '-' && (c < '0' || c > '9')) || !fresh_for_read())
        return dstd::nullopt;
    double out;
    const simdjson::error_code code =
        Access::inner(*this).get_double().get(out);
    if (code == simdjson::NUMBER_ERROR) {
        if (const auto refused = detail::refused_number(raw())) {
            state_ = SPENT;
            return refused;
        }
    }
    if (code) {
        owner_->broken = true;
        return dstd::nullopt;
    }
    state_ = SPENT;
    return out;
}

dstd::Optional<dstd::StringView> Value::as_string() {
    if (*start_ != '"') return dstd::nullopt;
    const auto out = detail::read_string(
        detail::value_extent(start_, owner_->end()), owner_->strings);
    if (!out) owner_->broken = true;
    return out;
}

dstd::Optional<bool> Value::as_bool() {
    if ((*start_ != 't' && *start_ != 'f') || !fresh_for_read())
        return dstd::nullopt;
    bool out;
    if (Access::inner(*this).get_bool().get(out)) {
        owner_->broken = true;
        return dstd::nullopt;
    }
    state_ = SPENT;
    return out;
}

bool Value::fresh_for_read() {
    if (!usable(*owner_)) return false;
    if (!root_) {
        owner_->disturbed = true;
        return state_ == FRESH;
    }
    if (state_ == FRESH && !owner_->disturbed) return true;
    return restart_root();
}

bool Value::restart_root() {
    if (!usable(*owner_) || owner_->walking > 0) return false;
    owner_->doc.rewind();
    OnDemand root;
    if (!note(*owner_, owner_->doc.get_value().get(root))) return false;
    ::new (static_cast<void*>(storage_)) OnDemand(root);
    owner_->disturbed = false;
    state_ = FRESH;
    return true;
}

}  // namespace dftracer::utils::json
