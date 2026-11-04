// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_EVALUATOR_H
#define DFTRACER_UTILS_DUQL_EVALUATOR_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/duql/detail/ast.h>
#include <dftracer/utils/duql/value_map.h>
#include <dftracer/utils/json/json_value.h>

#include <cstdint>

namespace dftracer::utils::duql {

using json::JsonValue;

/// A three-valued result: a leaf on a missing or null field, or across types,
/// is UNKNOWN; `and`, `or` and `not` follow Kleene logic.
enum class Truth : std::uint8_t { No, Yes, Unknown };

/// The result of `node` on a JSON record. With `args_fallback`, a bare name
/// the record lacks reads the field of that name under `args`.
Truth evaluate_truth(const detail::QueryNode& node, const JsonValue& event,
                     bool args_fallback = true);

/// The result of `node` on a flattened record.
Truth evaluate_truth(const detail::QueryNode& node, const ValueMap& fields);

/// Whether a filter keeps the record: evaluate_truth(...) == Truth::YES.
bool evaluate(const detail::QueryNode& node, const JsonValue& event,
              bool args_fallback = true);
bool evaluate(const detail::QueryNode& node, const ValueMap& fields);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_EVALUATOR_H
