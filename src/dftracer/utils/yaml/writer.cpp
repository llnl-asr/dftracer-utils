// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/yaml/escape.h>
#include <dftracer/utils/yaml/writer.h>

#include <cmath>
#include <cstring>

#ifdef DFTRACER_UTILS_EXPECTED_CHECKS
#define DFTRACER_UTILS_YAML_CHECK(condition, what) \
    do {                                           \
        if (!(condition)) dstd::fatal(what);       \
    } while (false)
#else
#define DFTRACER_UTILS_YAML_CHECK(condition, what) ((void)0)
#endif

namespace dftracer::utils::yaml {

namespace {

// The longest implicit key YAML allows is 1024 characters; the check counts
// bytes of the written form, which is never fewer.
constexpr std::size_t MAX_IMPLICIT_KEY = 1024;

constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

constexpr char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equals_ignoring_case(dstd::StringView s, dstd::StringView lower_name) {
    if (s.size() != lower_name.size()) return false;
    for (std::size_t i = 0; i < s.size(); ++i)
        if (lower(s[i]) != lower_name[i]) return false;
    return true;
}

bool starts_with(dstd::StringView s, dstd::StringView prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// A character a number, a timestamp or a sexagesimal in YAML 1.1 or 1.2 is
// spelled with.
bool is_number_character(char c) {
    if (is_digit(c)) return true;
    switch (c) {
        case 'a':
        case 'b':
        case 'B':
        case 'c':
        case 'd':
        case 'e':
        case 'f':
        case 'A':
        case 'C':
        case 'D':
        case 'E':
        case 'F':
        case 'x':
        case 'X':
        case 'o':
        case 'O':
        case '_':
        case ':':
        case '.':
        case '+':
        case '-':
            return true;
        default:
            return false;
    }
}

// True when a reader may take the plain form of `s` for something other than
// the string: a null, a bool, a number, a timestamp, a merge or a value key.
bool resolves_to_non_string(dstd::StringView s) {
    static constexpr dstd::StringView WORDS[] = {"~",   "null", "true", "false",
                                                 "yes", "no",   "on",   "off",
                                                 "y",   "n",    "<<",   "="};
    for (const dstd::StringView w : WORDS)
        if (equals_ignoring_case(s, w)) return true;
    dstd::StringView rest = s;
    if (!rest.empty() && (rest.front() == '+' || rest.front() == '-'))
        rest.remove_prefix(1);
    if (equals_ignoring_case(rest, ".inf") ||
        equals_ignoring_case(rest, ".nan"))
        return true;
    const char first = s.front();
    if (is_digit(first) || first == '+' || first == '.') {
        bool all = true;
        for (const char c : s) all = all && is_number_character(c);
        if (all) return true;
    }
    // YYYY-MM-DD, alone or followed by a time.
    if (s.size() >= 5 && is_digit(s[0]) && is_digit(s[1]) && is_digit(s[2]) &&
        is_digit(s[3]) && s[4] == '-')
        return true;
    return false;
}

bool is_indicator(char c) {
    switch (c) {
        case '-':
        case '?':
        case ':':
        case ',':
        case '[':
        case ']':
        case '{':
        case '}':
        case '#':
        case '&':
        case '*':
        case '!':
        case '|':
        case '>':
        case '\'':
        case '"':
        case '%':
        case '@':
        case '`':
            return true;
        default:
            return false;
    }
}

// True when the plain form of `s` is not safe: it needs the double quotes.
bool needs_quotes(dstd::StringView s) {
    if (s.empty()) return true;
    if (is_indicator(s.front()) || s.front() == ' ' || s.back() == ' ')
        return true;
    if (starts_with(s, "---") || starts_with(s, "...")) return true;
    if (s.back() == ':') return true;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char u = static_cast<unsigned char>(s[i]);
        if (u < 0x20 || u == 0x7F) return true;
        if (s[i] == ':' && i + 1 < s.size() && s[i + 1] == ' ') return true;
        if (s[i] == '#' && i > 0 && s[i - 1] == ' ') return true;
    }
    // Anything the escape would change is not plain either.
    if (escaped_yaml_size(s) != s.size()) return true;
    return resolves_to_non_string(s);
}

void append_double(dstd::String& out, double v) {
    if (std::isnan(v)) {
        out += ".nan";
        return;
    }
    if (std::isinf(v)) {
        out += v < 0 ? "-.inf" : ".inf";
        return;
    }
    constexpr std::size_t CAPACITY = 40;
    const std::size_t at = out.size();
    out.resize(at + CAPACITY);
    char* const begin = out.data() + at;
    char* const end = dstd::to_chars_double(begin, begin + CAPACITY - 2, v);
    std::size_t length = static_cast<std::size_t>(end - begin);
    std::size_t exponent = length;
    bool has_point = false;
    for (std::size_t i = 0; i < length; ++i) {
        if (begin[i] == '.') has_point = true;
        if (begin[i] == 'e') {
            exponent = i;
            break;
        }
    }
    if (!has_point) {
        std::memmove(begin + exponent + 2, begin + exponent, length - exponent);
        begin[exponent] = '.';
        begin[exponent + 1] = '0';
        length += 2;
    }
    out.resize(at + length);
}

}  // namespace

Writer::Writer(dstd::String& out, WriterOptions options)
    : out_(out), indent_(options.indent) {}

Writer::~Writer() {
    DFTRACER_UTILS_YAML_CHECK(frames_.empty(),
                              "yaml::Writer: destroyed with a container open");
}

void Writer::newline_indent(std::size_t column) {
    if (started_) out_ += '\n';
    out_.append(column, ' ');
    started_ = true;
}

void Writer::start_item() {
    if (inline_next_)
        inline_next_ = false;
    else
        newline_indent(frames_.back().column);
}

void Writer::begin(bool is_array) {
    std::size_t column = 0;
    if (frames_.empty()) {
        DFTRACER_UTILS_YAML_CHECK(!root_done_, "yaml::Writer: a second root");
    } else if (frames_.back().is_array) {
        start_item();
        out_ += "- ";
        inline_next_ = true;
        column = frames_.back().column + 2;
    } else {
        DFTRACER_UTILS_YAML_CHECK(frames_.back().key_pending,
                                  "yaml::Writer: an object member needs a key");
        column = frames_.back().column + indent_;
        if (frames_.back().explicit_key) {
            newline_indent(frames_.back().column);
            out_ += ':';
        }
    }
    frames_.push_back({column, 0, is_array, false, false});
}

void Writer::end(bool is_array) {
    DFTRACER_UTILS_YAML_CHECK(!frames_.empty(),
                              "yaml::Writer: end with nothing open");
    DFTRACER_UTILS_YAML_CHECK(frames_.back().is_array == is_array,
                              "yaml::Writer: end of the other container kind");
    DFTRACER_UTILS_YAML_CHECK(!frames_.back().key_pending,
                              "yaml::Writer: a key without a value");
    const Frame done = frames_.back();
    frames_.pop_back();
    if (done.count == 0) {
        inline_next_ = false;
        if (!frames_.empty() && !frames_.back().is_array) out_ += ' ';
        out_ += is_array ? "[]" : "{}";
        started_ = true;
    }
    scalar_done();
}

void Writer::begin_object() { begin(false); }
void Writer::end_object() { end(false); }
void Writer::begin_array() { begin(true); }
void Writer::end_array() { end(true); }

void Writer::write_string(dstd::StringView v) {
    if (needs_quotes(v))
        dstd::fmt_append_with<YamlEscape, "\"{}\"">(out_, v);
    else
        out_ += v;
}

void Writer::key(dstd::StringView k) {
    DFTRACER_UTILS_YAML_CHECK(!frames_.empty() && !frames_.back().is_array,
                              "yaml::Writer: a key outside an object");
    DFTRACER_UTILS_YAML_CHECK(!frames_.back().key_pending,
                              "yaml::Writer: two keys in a row");
    start_item();
    const std::size_t written =
        needs_quotes(k) ? escaped_yaml_size(k) + 2 : k.size();
    const bool is_explicit = written > MAX_IMPLICIT_KEY;
    if (is_explicit) out_ += "? ";
    write_string(k);
    if (!is_explicit) out_ += ':';
    frames_.back().key_pending = true;
    frames_.back().explicit_key = is_explicit;
}

void Writer::scalar_token_begin() {
    if (frames_.empty()) {
        DFTRACER_UTILS_YAML_CHECK(!root_done_, "yaml::Writer: a second root");
    } else if (frames_.back().is_array) {
        start_item();
        out_ += "- ";
    } else {
        DFTRACER_UTILS_YAML_CHECK(frames_.back().key_pending,
                                  "yaml::Writer: a value without a key");
        if (frames_.back().explicit_key) {
            newline_indent(frames_.back().column);
            out_ += ": ";
        } else {
            out_ += ' ';
        }
    }
}

void Writer::scalar_done() {
    started_ = true;
    if (frames_.empty()) {
        root_done_ = true;
        return;
    }
    Frame& top = frames_.back();
    top.key_pending = false;
    top.explicit_key = false;
    ++top.count;
}

void Writer::value(dstd::StringView v) {
    scalar_token_begin();
    write_string(v);
    scalar_done();
}

void Writer::value(const char* v) { value(dstd::StringView(v)); }

void Writer::value(double v) {
    scalar_token_begin();
    append_double(out_, v);
    scalar_done();
}

void Writer::value(bool v) {
    scalar_token_begin();
    out_ += v ? "true" : "false";
    scalar_done();
}

void Writer::value(std::int64_t v) {
    scalar_token_begin();
    dstd::fmt_append<"{}">(out_, v);
    scalar_done();
}

void Writer::value(std::uint64_t v) {
    scalar_token_begin();
    dstd::fmt_append<"{}">(out_, v);
    scalar_done();
}

void Writer::null() {
    scalar_token_begin();
    out_ += "null";
    scalar_done();
}

void Writer::raw_value(dstd::StringView token) {
    scalar_token_begin();
    out_ += token;
    scalar_done();
}

}  // namespace dftracer::utils::yaml
