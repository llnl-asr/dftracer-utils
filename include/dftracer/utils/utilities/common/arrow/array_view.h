#ifndef DFTRACER_UTILS_UTILITIES_COMMON_ARROW_ARRAY_VIEW_H
#define DFTRACER_UTILS_UTILITIES_COMMON_ARROW_ARRAY_VIEW_H

#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <nanoarrow/nanoarrow.h>

namespace dftracer::utils::utilities::common::arrow {

// Init `view` from `schema` and bind it to `array`. On either failure the view
// is reset and the nanoarrow error code is returned; on success returns
// NANOARROW_OK with `view` ready for reads. Callers own the successful view and
// must ArrowArrayViewReset it themselves.
inline int init_array_view(ArrowArrayView& view, ArrowSchema* schema,
                           ArrowArray* array) {
    ArrowError error;
    int rc = ArrowArrayViewInitFromSchema(&view, schema, &error);
    if (rc != NANOARROW_OK) {
        ArrowArrayViewReset(&view);
        return rc;
    }
    rc = ArrowArrayViewSetArray(&view, array, &error);
    if (rc != NANOARROW_OK) {
        ArrowArrayViewReset(&view);
        return rc;
    }
    return NANOARROW_OK;
}

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW
#endif  // DFTRACER_UTILS_UTILITIES_COMMON_ARROW_ARRAY_VIEW_H
