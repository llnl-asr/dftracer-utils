#ifndef DFTRACER_UTILS_DUQL_EVALUATOR_H
#define DFTRACER_UTILS_DUQL_EVALUATOR_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/duql/ast.h>
#include <dftracer/utils/json/json_value.h>

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

namespace dftracer::utils::duql {

using json::JsonValue;

/// A three-valued result: a leaf on a missing or null field, or across types,
/// is UNKNOWN; `and`, `or` and `not` follow Kleene logic.
enum class Truth : std::uint8_t { NO, YES, UNKNOWN };

/// A present field of a flattened record: a scalar, JSON `null`, or an array
/// or object held as its canonical JSON text in `value`.
struct Cell {
    enum class Kind : std::uint8_t { VALUE, NULL_VALUE, ARRAY, OBJECT };

    LiteralValue value;
    Kind kind = Kind::VALUE;

    Cell() = default;
    template <class T>
        requires(!std::is_same_v<std::decay_t<T>, Cell> &&
                 std::is_constructible_v<LiteralValue, T &&>)
    Cell(T&& v) : value(std::forward<T>(v)) {}

    static Cell null() {
        Cell c;
        c.kind = Kind::NULL_VALUE;
        return c;
    }
    /// `text` is canonical JSON (json::canonical_json_text).
    static Cell json(std::string text, bool array) {
        Cell c{std::move(text)};
        c.kind = array ? Kind::ARRAY : Kind::OBJECT;
        return c;
    }
};

/// Typed key-value map for non-JSON evaluation contexts. A key absent from
/// the map is a missing field.
using ValueMap = dftracer::utils::StringViewMap<Cell>;

/// The result of `node` on a JSON record. With `args_fallback`, a bare name
/// the record lacks reads the field of that name under `args`.
Truth evaluate_truth(const QueryNode& node, const JsonValue& event,
                     bool args_fallback = true);

/// The result of `node` on a flattened record.
Truth evaluate_truth(const QueryNode& node, const ValueMap& fields);

/// Whether a filter keeps the record: evaluate_truth(...) == Truth::YES.
bool evaluate(const QueryNode& node, const JsonValue& event,
              bool args_fallback = true);
bool evaluate(const QueryNode& node, const ValueMap& fields);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_EVALUATOR_H
