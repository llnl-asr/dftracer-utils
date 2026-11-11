// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_YAML_WRITER_H
#define DFTRACER_UTILS_YAML_WRITER_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace dftracer::utils::yaml {

struct WriterOptions {
    /// Columns a nested object or array is indented by.
    std::uint8_t indent = 2;
};

/// Writes block-style YAML into a string the caller owns, one event at a time:
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
/// A nested object or array starts on the line after its key, indented; an
/// array item is `- `, and an object in an array item starts on that line. An
/// empty object is `{}` and an empty array `[]`. The text has no trailing
/// newline, and the first line starts at the end of `out`.
///
/// A string is written plain only when no YAML 1.1 or 1.2 reader can take it
/// for a null, a bool, a number, a timestamp or markup; otherwise it is
/// double-quoted with the escapes of @ref escape_yaml_to. Keys follow the same
/// rule, so the key `1` or `yes` comes back as a string. YAML limits an
/// implicit key to 1024 characters, so a key whose written form is over 1024
/// bytes is written as an explicit key: `? key` on its own line, then `:` and
/// the value at the same indent. A double always has a
/// decimal point (`1.0`, `1.0e+20`) so it reads back as a float; the
/// non-finite ones are `.nan`, `.inf` and `-.inf`.
///
/// A call that breaks the grammar (a value in an object without a key, a key
/// in an array, an end with nothing open, a second root, a document that
/// stops with a container open) is a caller bug: it ends the process in a
/// build with DFTRACER_UTILS_DEBUG and is not checked in a release build.
class Writer {
   public:
    explicit Writer(dstd::String& out, WriterOptions options = {});
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
    void value(const char* v);
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
        requires(std::is_pointer_v<T> && !std::is_same_v<T, const char*>)
    void value(T) = delete;
    void null();

    /// One scalar token written as is. The caller guarantees it is a single
    /// valid YAML scalar on one line.
    void raw_value(dstd::StringView token);

    /// `key(k)` and `value(v)`.
    template <class V>
    void member(dstd::StringView k, const V& v) {
        key(k);
        value(v);
    }

   private:
    struct Frame {
        std::size_t column;  // where its items start
        std::size_t count;   // items written
        bool is_array;
        bool key_pending;
        bool explicit_key;   // the pending key is written as `? key`
    };

    void begin(bool is_array);
    void end(bool is_array);
    void start_item();
    void scalar_token_begin();
    void scalar_done();
    void write_string(dstd::StringView v);
    void newline_indent(std::size_t column);

    dstd::String& out_;
    dstd::Vector<Frame> frames_;
    std::uint8_t indent_;
    bool started_ = false;
    bool inline_next_ = false;
    bool root_done_ = false;
};

}  // namespace dftracer::utils::yaml

#endif  // DFTRACER_UTILS_YAML_WRITER_H
