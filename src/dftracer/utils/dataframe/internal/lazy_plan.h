// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_LAZY_PLAN_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_LAZY_PLAN_H

#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/function_ref.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace dftracer::utils::dataframe::detail {

enum class PlanRule : std::uint8_t {
    PredicatePushdown,
    ProjectionPushdown,
    SourceAbsorption
};

// The rules collect(), stream(), open_cursor() and explain() run, in order.
inline constexpr std::array<PlanRule, 3> PLAN_PIPELINE{
    PlanRule::PredicatePushdown, PlanRule::ProjectionPushdown,
    PlanRule::SourceAbsorption};

const char* plan_rule_name(PlanRule rule) noexcept;

// `lf` with one rule applied to its source and op list. Child plans are
// untouched.
LazyFrame apply_plan_rule(const LazyFrame& lf, PlanRule rule);

// `lf` with every PLAN_PIPELINE rule applied in order.
LazyFrame optimize_plan(const LazyFrame& lf);

// Structural hash of source identity, memory budget, every op and every child
// plan. Opaque payloads (mask series, is_in sets, plugin nodes, frame-op
// scalar arguments) hash by identity, so two equivalent plans may differ.
std::uint64_t plan_fingerprint(const LazyFrame& lf);

// The C++ callbacks of one run of a tap: `rows` on each morsel (its columns
// under their names), `end` once when the input is drained. An error from
// either fails the run.
struct TapRun {
    dstd::Function<dstd::Result<void>(const DataFrame&)> rows;
    dstd::Function<dstd::Result<void>()> end;
};

// A tap: `open` gives the callbacks of each run of the plan. A tap is an
// optimizer barrier.
struct Tap {
    dstd::Function<TapRun()> open;
};

// `lf` with every morsel shown to `tap` and passed on unchanged.
LazyFrame tap(const LazyFrame& lf, dstd::SharedPtr<const Tap> tap);

// The plan's leaf source.
const dstd::SharedPtr<const Source>& plan_source(const LazyFrame& lf);

// `lf`'s ops and memory budget over `source` instead of its own source.
LazyFrame rebase(const LazyFrame& lf, dstd::SharedPtr<const Source> source);

// explain()'s description of the first op in `lf` that is not a filter, or
// nullopt when every op is a filter.
dstd::Optional<dstd::String> first_non_filter_op(const LazyFrame& lf);

// explain()'s description of the first op in `lf`, or nullopt when it has none.
dstd::Optional<dstd::String> first_op(const LazyFrame& lf);

// (offset, len) when `lf`'s only op is a row window (head / slice / limit).
dstd::Optional<dstd::Pair<std::int64_t, std::int64_t>> sole_slice(
    const LazyFrame& lf);

// Calls `f` on `lf`, then depth first on every child plan: the other side of a
// join or concat and each frame operand of a frame op.
void visit_plan(const LazyFrame& lf,
                dstd::FunctionRef<void(const LazyFrame&, int depth)> f);

}  // namespace dftracer::utils::dataframe::detail

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_LAZY_PLAN_H
