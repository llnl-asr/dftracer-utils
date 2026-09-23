#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_LAZYFRAME_HANDLE_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_LAZYFRAME_HANDLE_H

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/lazyframe.h>

namespace dftracer::utils::dataframe {

// Access the LazyFrame wrapped by a dftu_lazyframe handle. Internal bridge so a
// consumer in another translation unit (the Python plugin result path) can move
// the plan into a native wrapper without re-declaring the opaque handle struct.
LazyFrame& lazyframe_handle_unwrap(dftu_lazyframe* h);

// Wrap a LazyFrame as a new owned dftu_lazyframe handle. Internal bridge so a
// producer in another translation unit (the trace View C ABI) can hand out a
// handle without re-declaring the opaque struct. Caller owns the result; free
// with dftu_lazyframe_free.
dftu_lazyframe* lazyframe_handle_wrap(LazyFrame&& lf);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_LAZYFRAME_HANDLE_H
