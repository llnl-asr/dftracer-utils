// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_SINK_PLAN_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_SINK_PLAN_H

#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>

namespace dftracer::utils::dataframe {

/// `plan` wrapped so that collecting it writes every frame `plan` streams to
/// the sink registered as `name` and yields one row of `rows` and `batches`
/// instead. Binding happens when the wrapped plan is collected, so a plan can
/// be built before the plugin that registers the sink is loaded. The error is
/// a NotFound one naming the registered sinks when no sink has that name.
///
/// `counts` receives the same totals the summary row carries; null discards
/// them. @ref LazyFrame::sink and the duql executor both run through this, so
/// the engine has one sink path.
dstd::Result<LazyFrame> sink_plan(
    const LazyFrame& plan, dstd::StringView name,
    dstd::Vector<dstd::Pair<dstd::String, dstd::String>> options,
    std::int64_t batch_size, dstd::SharedPtr<SinkSummary> counts = nullptr);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_SINK_PLAN_H
