#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_DATAFRAME_HANDLE_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_DATAFRAME_HANDLE_H

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>

namespace dftracer::utils::dataframe {

// Wrap a DataFrame as a new owned dftu_dataframe handle. Internal bridge so a
// producer in another translation unit (the trace View C ABI) can hand out a
// handle without re-declaring the opaque struct. Caller owns the result; free
// with dftu_dataframe_free.
dftu_dataframe* dataframe_handle_wrap(DataFrame&& df);

// The reverse bridge: consume `h` (deleting it) and return its DataFrame by
// value. Lets a consumer in another translation unit (the plugin provider
// adapter) take ownership of a frame crossing the ABI without a per-column
// copy. `h` must not be used again after this call.
DataFrame dataframe_handle_take(dftu_dataframe* h);
// Borrow the DataFrame behind a live handle; valid while `h` is.
const DataFrame& dataframe_handle_view(const dftu_dataframe* h);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_DATAFRAME_HANDLE_H
