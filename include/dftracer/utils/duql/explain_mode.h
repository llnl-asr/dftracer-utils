// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_EXPLAIN_MODE_H
#define DFTRACER_UTILS_DUQL_EXPLAIN_MODE_H

#include <cstdint>

namespace dftracer::utils::duql {

/// The `explain` prefix of a duql program: none, `explain` (the plan) or
/// `explain analyze` (the plan, run, with what each stage measured).
enum class ExplainMode : std::uint8_t { None, Plan, Analyze };

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_EXPLAIN_MODE_H
