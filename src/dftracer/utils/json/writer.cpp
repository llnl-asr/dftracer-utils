// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/json/writer.h>

#include <cmath>
#include <cstring>

#ifdef DFTRACER_UTILS_EXPECTED_CHECKS
#define DFTRACER_UTILS_JSON_CHECK(condition, what) \
    do {                                           \
        if (!(condition)) dstd::fatal(what);       \
    } while (false)
#else
#define DFTRACER_UTILS_JSON_CHECK(condition, what) ((void)0)
#endif

namespace dftracer::utils::json {

namespace {

void append_double(dstd::String& out, double v) {
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

Writer::Writer(dstd::String& out) : out_(out) {}

Writer::~Writer() {
    DFTRACER_UTILS_JSON_CHECK(frames_.empty(),
                              "json::Writer: destroyed with a container open");
}

void Writer::start_value() {
    if (frames_.empty()) {
        DFTRACER_UTILS_JSON_CHECK(!root_done_, "json::Writer: a second root");
        return;
    }
    Frame& top = frames_.back();
    if (top.is_array) {
        if (top.count != 0) out_ += ',';
    } else {
        DFTRACER_UTILS_JSON_CHECK(top.key_pending,
                                  "json::Writer: a value without a key");
    }
}

void Writer::value_done() {
    if (frames_.empty()) {
        root_done_ = true;
        return;
    }
    Frame& top = frames_.back();
    ++top.count;
    top.key_pending = false;
}

void Writer::begin(bool is_array) {
    start_value();
    out_ += is_array ? '[' : '{';
    frames_.push_back({0, is_array, false});
}

void Writer::end(bool is_array) {
    DFTRACER_UTILS_JSON_CHECK(!frames_.empty(),
                              "json::Writer: end with nothing open");
    DFTRACER_UTILS_JSON_CHECK(frames_.back().is_array == is_array,
                              "json::Writer: end of the other container kind");
    DFTRACER_UTILS_JSON_CHECK(!frames_.back().key_pending,
                              "json::Writer: a key without a value");
    frames_.pop_back();
    out_ += is_array ? ']' : '}';
    value_done();
}

void Writer::begin_object() { begin(false); }
void Writer::end_object() { end(false); }
void Writer::begin_array() { begin(true); }
void Writer::end_array() { end(true); }

void Writer::key(dstd::StringView k) {
    DFTRACER_UTILS_JSON_CHECK(!frames_.empty() && !frames_.back().is_array,
                              "json::Writer: a key outside an object");
    DFTRACER_UTILS_JSON_CHECK(!frames_.back().key_pending,
                              "json::Writer: two keys in a row");
    if (frames_.back().count != 0) out_ += ',';
    out_ += '"';
    append_json_escaped(out_, k);
    out_ += "\":";
    frames_.back().key_pending = true;
}

void Writer::value(dstd::StringView v) {
    start_value();
    out_ += '"';
    append_json_escaped(out_, v);
    out_ += '"';
    value_done();
}

void Writer::value(double v) {
    DFTRACER_UTILS_JSON_CHECK(std::isfinite(v),
                              "json::Writer: a double that is not finite");
    start_value();
    append_double(out_, v);
    value_done();
}

void Writer::value(bool v) {
    start_value();
    out_ += v ? "true" : "false";
    value_done();
}

void Writer::value(std::int64_t v) {
    start_value();
    dstd::fmt_append<"{}">(out_, v);
    value_done();
}

void Writer::value(std::uint64_t v) {
    start_value();
    dstd::fmt_append<"{}">(out_, v);
    value_done();
}

void Writer::null() {
    start_value();
    out_ += "null";
    value_done();
}

void Writer::raw_value(dstd::StringView token) {
    start_value();
    out_ += token;
    value_done();
}

}  // namespace dftracer::utils::json
