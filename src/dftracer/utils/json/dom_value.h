// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_JSON_DOM_VALUE_H
#define DFTRACER_UTILS_JSON_DOM_VALUE_H

#include <dftracer/utils/dstd/function_ref.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::json {

namespace detail {
struct DomState;
}

/// A value of a @ref DomDocument: a small handle that holds no copy. It and
/// every view it gives stay valid until the next parse by its document or the
/// destruction of the document. A call of the wrong kind is a caller bug: read
/// @ref type first.
class DomValue {
   public:
    /// An integer past INT64_MAX that fits uint64 is Uint; a number with a
    /// fraction, an exponent or a larger magnitude is Double.
    enum class Type : std::uint8_t {
        Object,
        Array,
        String,
        Int,
        Uint,
        Double,
        Bool,
        Null
    };

    Type type() const;

    /// The member `key` of an object, or empty when this is not an object or
    /// has no such member. A name that repeats gives the first.
    dstd::Optional<DomValue> find(dstd::StringView key) const;

    /// Calls `fn` with each member name and value of an object, in order. A
    /// name that repeats is visited each time. The name is decoded.
    void for_each_member(
        dstd::FunctionRef<void(dstd::StringView, DomValue)> fn) const;
    /// Calls `fn` with each element of an array, in order.
    void for_each_element(dstd::FunctionRef<void(DomValue)> fn) const;

    std::int64_t as_int() const;
    std::uint64_t as_uint() const;
    double as_double() const;
    bool as_bool() const;
    /// The decoded string.
    dstd::StringView as_string() const;

   private:
    friend class DomDocument;
    friend struct detail::DomState;

    alignas(8) unsigned char storage_[16];
};

/// Reads one whole JSON text with the DOM parser, which checks every byte in
/// one pass. @ref Document cannot serve a document read in full, because it
/// is lazy: it checks and decodes only the parts that are read, and accepts a
/// trailing comma or trailing text. This class refuses a trailing comma, text
/// after the value, a bad atom or number and a bad escape anywhere in the text.
/// Use it for a document read in full; use
/// @ref Document for a line of which a few members are read. Move only. Not
/// thread safe.
class DomDocument {
   public:
    DomDocument(DomDocument&&) noexcept;
    DomDocument& operator=(DomDocument&&) noexcept;
    ~DomDocument();
    DomDocument(const DomDocument&) = delete;
    DomDocument& operator=(const DomDocument&) = delete;

    /// Parses `text`, which is copied, and which must be one object or array.
    /// A text that is refused is an error with code Parse and the message
    /// `line:column: reason`, with a 1-based line and byte column. The place is
    /// found only after the parse refused the text, so a good text is read
    /// once.
    static dstd::Result<DomDocument> parse(dstd::StringView text);

    DomValue root() const;

   private:
    explicit DomDocument(dstd::UniquePtr<detail::DomState> state);

    dstd::UniquePtr<detail::DomState> state_;
};

}  // namespace dftracer::utils::json

#endif  // DFTRACER_UTILS_JSON_DOM_VALUE_H
