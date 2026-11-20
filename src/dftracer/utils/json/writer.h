// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_JSON_WRITER_H
#define DFTRACER_UTILS_JSON_WRITER_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dftracer::utils::json {

/// Writes compact JSON into a string the caller owns, one event at a time:
///
///   Writer w(out);
///   w.begin_object();
///   w.member("name", "demo");
///   w.key("sizes");
///   w.begin_array();
///   w.value(std::int64_t{1});
///   w.end_array();
///   w.end_object();
///
/// Members and items are separated by `,` and a key by `:`, with no white
/// space, and the text starts at the end of `out`. A string is escaped as
/// @ref append_json_escaped does. A double always has a decimal point or an
/// exponent, so it reads back as a double, and a double that is not finite is a
/// caller bug, as JSON has no spelling for it.
///
/// A call that breaks the grammar (a value in an object without a key, a key in
/// an array, an end with nothing open, a second root, a document that stops
/// with a container open, a double that is not finite) is a caller bug: it ends
/// the process in a build with DFTRACER_UTILS_DEBUG and is not checked in a
/// release build.
class Writer {
   public:
    explicit Writer(dstd::String& out);
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    ~Writer();

    void begin_object();
    void end_object();
    void begin_array();
    void end_array();

    /// The key of the next member of an object.
    void key(dstd::StringView k);

    void value(dstd::StringView v);
    template <std::size_t N>
    void value(const char (&v)[N]) {
        value(dstd::StringView(v));
    }
    void value(double v);
    void value(bool v);
    void value(std::int64_t v);
    void value(std::uint64_t v);
    void value(char) = delete;
    template <class T>
        requires(std::is_integral_v<T> && !std::is_same_v<T, bool> &&
                 !std::is_same_v<T, char> && !std::is_same_v<T, std::int64_t> &&
                 !std::is_same_v<T, std::uint64_t>)
    void value(T v) {
        if constexpr (std::is_signed_v<T>)
            value(static_cast<std::int64_t>(v));
        else
            value(static_cast<std::uint64_t>(v));
    }
    template <class T>
        requires(std::is_pointer_v<T>)
    void value(T) = delete;
    void null();

    /// One value written as is. The caller guarantees it is valid JSON.
    void raw_value(dstd::StringView token);

    /// `key(k)` and `value(v)`.
    template <class V>
    void member(dstd::StringView k, const V& v) {
        key(k);
        value(v);
    }

   private:
    struct Frame {
        std::size_t count;  // items written
        bool is_array;
        bool key_pending;
    };

    void begin(bool is_array);
    void end(bool is_array);
    void start_value();
    void value_done();

    dstd::String& out_;
    dstd::Vector<Frame> frames_;
    bool root_done_ = false;
};

}  // namespace dftracer::utils::json

#endif  // DFTRACER_UTILS_JSON_WRITER_H
