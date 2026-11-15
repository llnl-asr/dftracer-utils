// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/json/dom_value.h>
#include <simdjson.h>

#include <cstring>
#include <utility>

namespace dftracer::utils::json {

namespace detail {

struct DomState {
    simdjson::dom::parser parser;
    simdjson::dom::element root;

    static DomValue wrap(simdjson::dom::element element) {
        DomValue value;
        std::memcpy(value.storage_, &element, sizeof(element));
        return value;
    }
    static simdjson::dom::element unwrap(const DomValue& value) {
        simdjson::dom::element element;
        std::memcpy(&element, value.storage_, sizeof(element));
        return element;
    }
};

}  // namespace detail

namespace {

static_assert(sizeof(simdjson::dom::element) <= 16);
static_assert(alignof(simdjson::dom::element) <= 8);

constexpr int MAX_DEPTH = 1000;

// What the first syntax error of a JSON text is, found only after the reader
// refused the text: the reader does not say where.
struct JsonFault {
    std::size_t offset = 0;
    const char* reason = nullptr;
};

class JsonChecker {
   public:
    explicit JsonChecker(dstd::StringView text) : text_(text) {}

    // True when the text is one JSON object or array and nothing else.
    bool check() {
        skip_space();
        if (pos_ >= text_.size()) return fail("empty document");
        if (text_[pos_] != '{' && text_[pos_] != '[')
            return fail("the root must be an object or an array");
        if (!value(0)) return false;
        skip_space();
        if (pos_ < text_.size()) return fail("unexpected text after the value");
        return true;
    }

    JsonFault fault() const { return fault_; }

   private:
    bool fail(const char* reason) {
        fault_ = {pos_, reason};
        return false;
    }

    void skip_space() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                text_[pos_] == '\n' || text_[pos_] == '\r'))
            ++pos_;
    }

    bool literal(dstd::StringView word) {
        if (text_.substr(pos_, word.size()) != word)
            return fail("invalid value");
        pos_ += word.size();
        return true;
    }

    bool digits() {
        const std::size_t start = pos_;
        while (pos_ < text_.size() && dstd::is_ascii_digit(text_[pos_])) ++pos_;
        return pos_ > start ? true : fail("a digit is missing");
    }

    bool number() {
        const std::size_t start = pos_;
        bool integer = true;
        if (text_[pos_] == '-') ++pos_;
        const std::size_t digits_start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '0') {
            ++pos_;
        } else if (!digits()) {
            return false;
        }
        const std::size_t digits_end = pos_;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            integer = false;
            ++pos_;
            if (!digits()) return false;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            integer = false;
            ++pos_;
            if (pos_ < text_.size() &&
                (text_[pos_] == '+' || text_[pos_] == '-'))
                ++pos_;
            if (!digits()) return false;
        }
        if (integer) {
            const dstd::StringView digits_text =
                text_.substr(digits_start, digits_end - digits_start);
            const dstd::StringView limit = text_[start] == '-'
                                               ? "9223372036854775808"
                                               : "18446744073709551615";
            if (digits_text.size() > limit.size() ||
                (digits_text.size() == limit.size() && digits_text > limit)) {
                pos_ = start;
                return fail("an integer wider than 64 bits");
            }
        } else if (!dstd::from_chars<double>(
                       text_.substr(start, pos_ - start))) {
            pos_ = start;
            return fail("a number out of the range of a double");
        }
        return true;
    }

    // The length of the well-formed UTF-8 sequence at the current byte, or 0.
    std::size_t utf8_length() const {
        const auto at = [&](std::size_t i) {
            return pos_ + i < text_.size()
                       ? static_cast<unsigned char>(text_[pos_ + i])
                       : static_cast<unsigned char>(0);
        };
        const unsigned char lead = at(0);
        std::size_t length = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            if (lead == 0xE0) low = 0xA0;
            if (lead == 0xED) high = 0x9F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            if (lead == 0xF0) low = 0x90;
            if (lead == 0xF4) high = 0x8F;
        } else {
            return 0;
        }
        if (at(1) < low || at(1) > high) return 0;
        for (std::size_t i = 2; i < length; ++i)
            if (at(i) < 0x80 || at(i) > 0xBF) return 0;
        return length;
    }

    bool string() {
        ++pos_;
        while (pos_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20) return fail("a control character in a string");
            if (c >= 0x80) {
                const std::size_t length = utf8_length();
                if (length == 0) return fail("invalid UTF-8");
                pos_ += length;
                continue;
            }
            if (c == '\\') {
                ++pos_;
                if (pos_ >= text_.size()) break;
                const char e = text_[pos_];
                if (e == 'u') {
                    for (int i = 1; i <= 4; ++i)
                        if (pos_ + i >= text_.size() ||
                            !dstd::is_ascii_xdigit(text_[pos_ + i])) {
                            pos_ += i;
                            return fail("invalid \\u escape");
                        }
                    pos_ += 4;
                } else if (dstd::StringView("\"\\/bfnrt").find(e) ==
                           dstd::StringView::npos) {
                    return fail("invalid escape");
                }
            }
            ++pos_;
        }
        return fail("the string is not closed");
    }

    bool value(int depth) {
        skip_space();
        if (pos_ >= text_.size()) return fail("a value is missing");
        if (depth > MAX_DEPTH) return fail("nested too deep");
        const char c = text_[pos_];
        if (c == '{') return object(depth);
        if (c == '[') return array(depth);
        if (c == '"') return string();
        if (c == 't') return literal("true");
        if (c == 'f') return literal("false");
        if (c == 'n') return literal("null");
        if (c == '-' || dstd::is_ascii_digit(c)) return number();
        return fail("invalid value");
    }

    bool object(int depth) {
        ++pos_;
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_space();
            if (pos_ >= text_.size() || text_[pos_] != '"')
                return fail("a member name is missing");
            if (!string()) return false;
            skip_space();
            if (pos_ >= text_.size() || text_[pos_] != ':')
                return fail("a ':' is missing");
            ++pos_;
            if (!value(depth + 1)) return false;
            skip_space();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return fail("a ',' or a '}' is missing");
        }
    }

    bool array(int depth) {
        ++pos_;
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            if (!value(depth + 1)) return false;
            skip_space();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < text_.size() && text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return fail("a ',' or a ']' is missing");
        }
    }

    dstd::StringView text_;
    std::size_t pos_ = 0;
    JsonFault fault_;
};

dstd::Unexpected<dstd::Error> refused(dstd::StringView text,
                                      simdjson::error_code code) {
    JsonChecker checker(text);
    if (checker.check())
        return common::make_error<"invalid JSON: {}">(
            common::ErrorCode::Parse, simdjson::error_message(code));
    const JsonFault fault = checker.fault();
    std::size_t line = 1;
    std::size_t line_start = 0;
    for (std::size_t i = 0; i < fault.offset; ++i)
        if (text[i] == '\n') {
            ++line;
            line_start = i + 1;
        }
    return common::make_error<"{}:{}: {}">(common::ErrorCode::Parse, line,
                                           fault.offset - line_start + 1,
                                           dstd::StringView(fault.reason));
}

}  // namespace

DomValue::Type DomValue::type() const {
    switch (detail::DomState::unwrap(*this).type()) {
        case simdjson::dom::element_type::OBJECT:
            return Type::Object;
        case simdjson::dom::element_type::ARRAY:
            return Type::Array;
        case simdjson::dom::element_type::STRING:
            return Type::String;
        case simdjson::dom::element_type::INT64:
            return Type::Int;
        case simdjson::dom::element_type::UINT64:
            return Type::Uint;
        case simdjson::dom::element_type::DOUBLE:
            return Type::Double;
        case simdjson::dom::element_type::BOOL:
            return Type::Bool;
        case simdjson::dom::element_type::BIGINT:
        case simdjson::dom::element_type::NULL_VALUE:
            break;
    }
    return Type::Null;
}

dstd::Optional<DomValue> DomValue::find(dstd::StringView key) const {
    simdjson::dom::object object;
    if (detail::DomState::unwrap(*this).get_object().get(object))
        return dstd::nullopt;
    simdjson::dom::element element;
    if (object.at_key(key).get(element)) return dstd::nullopt;
    return detail::DomState::wrap(element);
}

void DomValue::for_each_member(
    dstd::FunctionRef<void(dstd::StringView, DomValue)> fn) const {
    simdjson::dom::object object;
    if (detail::DomState::unwrap(*this).get_object().get(object)) return;
    for (auto member : object)
        fn(dstd::StringView(member.key.data(), member.key.size()),
           detail::DomState::wrap(member.value));
}

void DomValue::for_each_element(dstd::FunctionRef<void(DomValue)> fn) const {
    simdjson::dom::array array;
    if (detail::DomState::unwrap(*this).get_array().get(array)) return;
    for (simdjson::dom::element element : array)
        fn(detail::DomState::wrap(element));
}

std::int64_t DomValue::as_int() const {
    return detail::DomState::unwrap(*this).get_int64().value_unsafe();
}

std::uint64_t DomValue::as_uint() const {
    return detail::DomState::unwrap(*this).get_uint64().value_unsafe();
}

double DomValue::as_double() const {
    return detail::DomState::unwrap(*this).get_double().value_unsafe();
}

bool DomValue::as_bool() const {
    return detail::DomState::unwrap(*this).get_bool().value_unsafe();
}

dstd::StringView DomValue::as_string() const {
    const auto text =
        detail::DomState::unwrap(*this).get_string().value_unsafe();
    return dstd::StringView(text.data(), text.size());
}

DomDocument::DomDocument(dstd::UniquePtr<detail::DomState> state)
    : state_(std::move(state)) {}
DomDocument::DomDocument(DomDocument&&) noexcept = default;
DomDocument& DomDocument::operator=(DomDocument&&) noexcept = default;
DomDocument::~DomDocument() = default;

dstd::Result<DomDocument> DomDocument::parse(dstd::StringView text) {
    auto state = dstd::make_unique<detail::DomState>();
    auto parsed = state->parser.parse(text.data(), text.size());
    if (parsed.error()) return refused(text, parsed.error());
    state->root = parsed.value_unsafe();
    if (!state->root.is_object() && !state->root.is_array())
        return refused(text, simdjson::SUCCESS);
    return DomDocument(std::move(state));
}

DomValue DomDocument::root() const {
    return detail::DomState::wrap(state_->root);
}

}  // namespace dftracer::utils::json
