// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_EVALUATOR_H
#define DFTRACER_UTILS_DUQL_EVALUATOR_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_arena.h>
#include <dftracer/utils/duql/detail/ast.h>
#include <dftracer/utils/duql/value_map.h>
#include <dftracer/utils/json/json_value.h>

#include <cstdint>
#include <cstring>

namespace dftracer::utils::duql {

using json::JsonValue;

/// A three-valued result: a leaf on a missing or null field, or across types,
/// is UNKNOWN; `and`, `or` and `not` follow Kleene logic.
enum class Truth : std::uint8_t { No, Yes, Unknown };

/// The result of `node` on a JSON record. With `args_fallback`, a bare name
/// the record lacks reads the field of that name under `args`.
Truth evaluate_truth(const detail::QueryNode& node, const JsonValue& event,
                     bool args_fallback = true);

/// A JSON line read on demand. `doc` refers to a parsed on-demand document of
/// `text`,
/// whose padded buffer must stay readable. Decoded strings and names go into
/// `strings`, which the caller clears per line: a view from it dies then. The
/// document is rewound before every read, so the caller can rewind it and read
/// again after an evaluation.
struct OnDemandEvent {
    mutable simdjson::ondemand::document_reference doc;
    dstd::StringView text;
    dstd::StringArena& strings;
    bool escapes;
    /// Set when a read hits a syntax error: the line is broken, the iterator
    /// is unusable and no later read counts. The caller drops the line.
    mutable bool broken = false;

    OnDemandEvent(simdjson::ondemand::document_reference document,
                  dstd::StringView line, dstd::StringArena& arena)
        : doc(document),
          text(line),
          strings(arena),
          escapes(std::memchr(line.data(), '\\', line.size()) != nullptr) {}
};

/// The result of `node` on an on-demand record, with the same answer as on the
/// DOM record of the same line.
Truth evaluate_truth(const detail::QueryNode& node, const OnDemandEvent& event,
                     bool args_fallback = true);
bool evaluate(const detail::QueryNode& node, const OnDemandEvent& event,
              bool args_fallback = true);

/// The result of `node` on a flattened record.
Truth evaluate_truth(const detail::QueryNode& node, const ValueMap& fields);

/// Whether a filter keeps the record: evaluate_truth(...) == Truth::YES.
bool evaluate(const detail::QueryNode& node, const JsonValue& event,
              bool args_fallback = true);
bool evaluate(const detail::QueryNode& node, const ValueMap& fields);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_EVALUATOR_H
