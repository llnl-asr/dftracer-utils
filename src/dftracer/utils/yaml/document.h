// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_YAML_DOCUMENT_H
#define DFTRACER_UTILS_YAML_DOCUMENT_H

#include <dftracer/utils/dstd/function_ref.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::yaml {

struct DocumentStorage;

/// A node of a @ref Document: an object, an array, a string or a null. It is
/// a view that holds no copy, so it and every view it returns stay valid until
/// the document is destroyed. A call on a node of the wrong kind returns
/// nothing, an empty view or an empty @ref dstd::Optional, never an error.
///
/// A scalar is untyped text, so every non-null scalar is a string and the
/// `as_` readers decide whether its text is a number or a bool.
class Value {
   public:
    bool is_object() const;
    bool is_array() const;
    /// A scalar with text: not an object, an array or a null. A quoted `"null"`
    /// is a string; an empty value, `~` and a bare `null` are not.
    bool is_string() const;
    bool is_null() const;

    /// The member of an object under `key`, or nothing when this is not an
    /// object or has no such key.
    dstd::Optional<Value> find(dstd::StringView key) const;

    /// Calls `fn` with the key and the value of every member of an object, in
    /// document order; a null key is empty. Does nothing for any other kind.
    void for_each_member(
        dstd::FunctionRef<void(dstd::StringView, Value)> fn) const;
    /// Calls `fn` with every element of an array, in document order. Does
    /// nothing for any other kind.
    void for_each_element(dstd::FunctionRef<void(Value)> fn) const;

    /// A string that is one decimal integer in the range of the type; a
    /// leading `+` is accepted. as_uint refuses a `-`.
    dstd::Optional<std::int64_t> as_int() const;
    dstd::Optional<std::uint64_t> as_uint() const;
    /// A string that is one decimal number, or .inf, -.inf or .nan in the
    /// lower case, upper case or capitalized form.
    dstd::Optional<double> as_double() const;
    /// A string spelled y, n, yes, no, true, false, on or off, in lower case,
    /// upper case or capitalized.
    dstd::Optional<bool> as_bool() const;
    /// The text of a string, with quotes and escapes resolved. A view, no copy.
    dstd::Optional<dstd::StringView> as_string() const;
    /// The text of a string, and empty for any other kind.
    dstd::StringView raw() const;

   private:
    friend class Document;
    friend class Records;
    friend struct LineAccess;

    Value(const DocumentStorage* storage, std::size_t id)
        : storage_(storage), id_(id) {}
    Value(const DocumentStorage* storage, std::uint64_t id, std::uint32_t col,
          std::uint8_t tag)
        : storage_(storage), id_(id), col_(col), tag_(tag) {}

    // A tape value: `id_` is the tape index. A line value: `id_` holds the
    // first line and the limit of a container, or the start and end of a
    // scalar; `col_` is the column of a container; `tag_` is its kind.
    const DocumentStorage* storage_ = nullptr;
    std::uint64_t id_ = 0;
    std::uint32_t col_ = 0;
    std::uint8_t tag_ = 0;
};

/// A parsed YAML or JSON text. It owns the text and the tape built over it, so
/// the strings of its values are views into that text, or into the arena of
/// the document for a string that grew while it was read.
class Document {
   public:
    Document(Document&&) noexcept;
    Document& operator=(Document&&) noexcept;
    ~Document();

    /// Parses the first document of `text`, which the document keeps: a string
    /// moved in is not copied. A text of several documents is read whole, and
    /// the root is the first. A syntax error is an InvalidArgument error whose
    /// message is `source_name:line:column: reason`, or `line:column: reason`
    /// when `source_name` is empty. Nothing throws, and a failed parse leaves
    /// no state behind. A mapping that holds a key twice, by the text of the
    /// key (`"a"` and `a` are the same key, and so are two aliases of one
    /// scalar), is an InvalidArgument error `source_name:line:column:
    /// duplicate key 'k'`, as YAML 1.2.2 requires unique keys. A container as
    /// a key, an alias to a node that holds it and aliases that expand to
    /// more than 2^27 nodes are InvalidArgument errors too. An alias reads as
    /// the node it names, with no copy; a `<<` key reads as the members of
    /// its maps, and a key of the map itself wins.
    static dstd::Result<Document> parse(dstd::String text,
                                        dstd::StringView source_name);

    /// Reads the file at `path` into the buffer the document owns and parses
    /// it, naming the file in an error.
    static dstd::Result<Document> load_file(dstd::PathView path);

    /// The top node. Empty text gives a null node.
    Value root() const;

   private:
    friend struct DocumentTestAccess;

    explicit Document(dstd::UniquePtr<DocumentStorage> storage);

    dstd::UniquePtr<DocumentStorage> storage_;
};

}  // namespace dftracer::utils::yaml

#endif  // DFTRACER_UTILS_YAML_DOCUMENT_H
