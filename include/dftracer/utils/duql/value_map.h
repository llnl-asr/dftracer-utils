// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_VALUE_MAP_H
#define DFTRACER_UTILS_DUQL_VALUE_MAP_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/duql/types.h>

#include <cstdint>
#include <type_traits>
#include <utility>

namespace dftracer::utils::duql {

/// A present field of a flattened record: a scalar, JSON `null`, or an array
/// or object held as its canonical JSON text in `value`.
struct Cell {
    enum class Kind : std::uint8_t { Value, NullValue, Array, Object };

    LiteralValue value;
    Kind kind = Kind::Value;

    Cell() = default;
    template <class T>
        requires(!std::is_same_v<std::decay_t<T>, Cell> &&
                 std::is_constructible_v<LiteralValue, T &&>)
    Cell(T&& v) : value(std::forward<T>(v)) {}

    static Cell null() {
        Cell c;
        c.kind = Kind::NullValue;
        return c;
    }
    /// `text` is canonical JSON (json::canonical_json_text).
    static Cell json(dstd::String text, bool array) {
        Cell c{std::move(text)};
        c.kind = array ? Kind::Array : Kind::Object;
        return c;
    }
};

/// Typed key-value map for non-JSON evaluation contexts. A key absent from
/// the map is a missing field.
using ValueMap = dftracer::utils::dstd::StringViewMap<Cell>;

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_VALUE_MAP_H
