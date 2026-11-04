// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_JSON_VALUE_H
#define DFTRACER_UTILS_JSON_VALUE_H

#include <dftracer/utils/dstd/function_ref.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::json {

class Document;
class Value;

namespace detail {
struct DocumentState;
struct ValueAccess;
}  // namespace detail

/// A JSON value read on demand: the text is scanned as you read it, and only
/// what you read is decoded. A Value comes from @ref Document::parse or from
/// another Value, and is a small handle that allocates nothing.
///
/// Reads go forward. @ref as_int, @ref as_double and @ref as_bool and the
/// iteration read through the parser, so each runs once per value: a second
/// call on the same value is empty, and @ref for_each_member and
/// @ref for_each_element visit once. Keep what you read. @ref type, @ref raw
/// and @ref as_string only look at the text, so they work on any value, as
/// often as you like, before or after the other reads.
///
/// The root value that @ref Document::parse returns is the exception to read
/// once: any read of it that needs a fresh position restarts the document, so
/// the root can be read again and again. A restart invalidates the child values
/// taken earlier, as a new parse would. A child Value is also invalid once its
/// parent moves on. @ref find of a member that comes before the current
/// position scans the object again, so it works but costs more. The views from
/// @ref as_string and @ref raw live until the next @ref Document::parse. Not
/// thread safe.
///
/// A Value is move-only. A copy would share one parser position with the
/// original, so reading through the copy would silently move the original.
class Value {
   public:
    /// The JSON type of a value. An integer outside the range of int64 is
    /// Double, so Int means that @ref as_int succeeds.
    enum class Type : std::uint8_t {
        Object,
        Array,
        String,
        Int,
        Double,
        Bool,
        Null
    };

    Value(Value&&) noexcept = default;
    Value& operator=(Value&&) noexcept = default;
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;
    ~Value() = default;

    /// The type of this value, from its text. It never reads through the
    /// parser, so it is right before and after any other read. Text that is not
    /// a JSON value reports Null, and every `as_` read of it is empty.
    Type type();

    /// The member `key` of an object, or empty when this is not an object or
    /// has no such member. `key` is compared with the name as it is written in
    /// the text, so a name that holds an escape such as `\"` is found by its
    /// escaped spelling; @ref for_each_member gives decoded names, or the raw
    /// text of a name that holds an invalid escape.
    dstd::Optional<Value> find(dstd::StringView key);

    /// Calls `fn` with each member name and value of an object, in order.
    /// Does nothing when this is not an object, or for a child value after
    /// @ref find or an earlier visit. The root value restarts instead, so
    /// `find` then `for_each_member` on the root visits every member. The name
    /// views live until the next @ref Document::parse. The value passed to `fn`
    /// is valid only inside that call, and `fn` must not read this value or
    /// its parent: a @ref find, a second walk or a query on it from inside `fn`
    /// comes back empty.
    void for_each_member(dstd::FunctionRef<void(dstd::StringView, Value)> fn);

    /// Calls `fn` with each element of an array, in order. Does nothing when
    /// this is not an array, or for a child value after an earlier visit. The
    /// value passed to `fn` is valid only inside that call, and `fn` must not
    /// read this value or its parent: such a read comes back empty.
    void for_each_element(dstd::FunctionRef<void(Value)> fn);

    /// The integer, or empty when this is not an integer or does not fit
    /// int64. An unsigned integer past INT64_MAX is empty; read it with
    /// @ref as_double.
    dstd::Optional<std::int64_t> as_int();
    /// The number as a double, or empty when this is not a number. An integer
    /// converts, and may lose precision past 2^53. A number too large for a
    /// double reads as +inf or -inf, one too small as a signed zero.
    dstd::Optional<double> as_double();
    /// The string with escapes decoded, or empty when this is not a string. A
    /// string with an invalid escape (`\q`, a short `\u12`, a lone surrogate)
    /// is the raw text between the quotes, undecoded. A string with no
    /// backslash is a view of the line itself. A decoded string lives in a
    /// buffer owned by the @ref Document, which a later read never moves.
    /// Either view lives until the next @ref Document::parse, however often
    /// you read.
    dstd::Optional<dstd::StringView> as_string();
    /// The boolean, or empty when this is not a boolean.
    dstd::Optional<bool> as_bool();

    /// The exact JSON text of this value, without the whitespace around it.
    /// It looks only at the text, so it works on any value at any time and
    /// does not spend the value. The view lives until the next
    /// @ref Document::parse.
    dstd::StringView raw();

   private:
    friend class Document;
    friend struct detail::ValueAccess;

    Value() = default;

    bool fresh_for_read();
    bool restart_root();

    static constexpr std::size_t STORAGE_SIZE = 24;

    alignas(8) unsigned char storage_[STORAGE_SIZE];
    detail::DocumentState* owner_ = nullptr;
    const char* start_ = nullptr;
    mutable std::uint8_t state_ = 0;
    bool root_ = false;
};

/// Reads JSON lines on demand. It owns the parser and one padded buffer that
/// every parse reuses. Make one per reader or thread, not one per line. Move
/// only. Not thread safe.
class Document {
   public:
    Document();
    ~Document();
    Document(Document&&) noexcept;
    Document& operator=(Document&&) noexcept;
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    /// Starts reading `line`, one JSON object or array. The parser needs
    /// padding after the text, so the line is copied into the reused buffer;
    /// once the buffer is large enough a parse allocates nothing in this class.
    /// Returns the root Value, or a @ref
    /// dftracer::utils::common::ErrorCode::Parse error when the line is empty,
    /// not JSON, or a bare scalar such as `7`.
    ///
    /// Parsing checks the structure of the whole line but decodes and checks
    /// only what you read: a bad atom, number or string escape, or a missing
    /// value, in a part that no read reaches does not make the line fail, and
    /// a full check would cost a second pass over every line. A read that does
    /// reach such a part marks the line broken: that read and every later read
    /// of the line comes back empty, except @ref Value::type, @ref Value::raw
    /// and @ref Value::as_string, which only look at the text.
    /// @ref Value::for_each_member and @ref Value::for_each_element visit
    /// nothing, and
    /// @ref dftracer::utils::duql::Query::matches returns false whatever the
    /// filter says, `not` and `or` included. A broken line is a dropped line.
    /// A key that is simply absent is not an error. A new parse clears it.
    ///
    /// Parsing again invalidates every Value and every string view from the
    /// previous parse.
    dstd::Result<Value> parse(dstd::StringView line);

   private:
    friend class Value;
    friend struct detail::ValueAccess;

    dstd::UniquePtr<detail::DocumentState> state_;
};

}  // namespace dftracer::utils::json

#endif  // DFTRACER_UTILS_JSON_VALUE_H
