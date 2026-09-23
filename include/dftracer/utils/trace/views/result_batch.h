#ifndef DFTRACER_UTILS_TRACE_VIEWS_RESULT_BATCH_H
#define DFTRACER_UTILS_TRACE_VIEWS_RESULT_BATCH_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/trace/views/view.h>

namespace dftracer::utils::trace::views {

/// Run a View aggregation and return its result as a columnar
/// dataframe::DataFrame. The one-call bridge from the View engine into the vec
/// kernels: `co_await` (or
/// `.get()`) it, then run vec ops over the columns.
inline coro::CoroTask<dftracer::utils::dataframe::DataFrame> collect_batch(
    const View& view) {
    co_return co_await view.collect().collect();
}

}  // namespace dftracer::utils::trace::views

#endif  // DFTRACER_UTILS_TRACE_VIEWS_RESULT_BATCH_H
