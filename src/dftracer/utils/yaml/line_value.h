// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_YAML_LINE_VALUE_H
#define DFTRACER_UTILS_YAML_LINE_VALUE_H

#include <dftracer/utils/dstd/function_ref.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/yaml/document.h>
#include <dftracer/utils/yaml/tape.h>

#include <cstdint>

namespace dftracer::utils::yaml {

/// What a @ref Value over a line index is.
enum class LineTag : std::uint8_t { Null, Map, Seq, Scalar };

/// The @ref Value operations over a @ref LineIndex. A value is a position in
/// the index: nothing here keeps state, so a value is a plain copy.
struct LineAccess {
    static Value make(const DocumentStorage* storage, LineTag tag,
                      std::uint64_t id, std::uint32_t col, bool arena = false);

    /// The root of the first document.
    static Value root(const DocumentStorage* storage);

    /// The next document after line `pos`, which moves past it. False after
    /// the last one.
    static bool next_document(const DocumentStorage* storage,
                              std::uint32_t& pos, std::uint32_t& first,
                              std::uint32_t& limit);

    /// The container that starts at the content line `first`, to `limit`.
    static Value container(const DocumentStorage* storage, std::uint32_t first,
                           std::uint32_t limit);

    /// The element of a sequence whose item line is `line`.
    static Value element(const DocumentStorage* storage, std::uint32_t line);

    static bool is_object(const Value& v);
    static bool is_array(const Value& v);
    static bool is_string(const Value& v);
    static bool is_null(const Value& v);
    static dstd::Optional<Value> find(const Value& v, dstd::StringView key);
    static void for_each_member(
        const Value& v, dstd::FunctionRef<void(dstd::StringView, Value)> fn);
    static void for_each_element(const Value& v,
                                 dstd::FunctionRef<void(Value)> fn);
    static dstd::StringView raw(const Value& v);
};

}  // namespace dftracer::utils::yaml

#endif  // DFTRACER_UTILS_YAML_LINE_VALUE_H
