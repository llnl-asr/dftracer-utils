// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_DUQL_EVAL_JSON_H
#define DFTRACER_UTILS_TESTS_DUQL_EVAL_JSON_H

#include <dftracer/utils/duql/detail/query_access.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/json/json_value.h>

/// Whether `q` keeps the JSON record `event`.
inline bool eval_json(const dftracer::utils::duql::Query& q,
                      const dftracer::utils::json::JsonValue& event,
                      bool args_fallback = true) {
    return dftracer::utils::duql::evaluate(
        dftracer::utils::duql::detail::QueryAccess::root(q), event,
        args_fallback);
}

#endif  // DFTRACER_UTILS_TESTS_DUQL_EVAL_JSON_H
