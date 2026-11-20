// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_OP_FAILURE_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_OP_FAILURE_H

#include <dftracer/utils/dstd/string.h>

namespace dftracer::utils::dataframe {

/// The reason a series op returns NULL, for the ops whose C entry has no error
/// channel and one known cause; nullptr for any other op.
inline const char* op_failure_reason(dstd::StringView op) noexcept {
    if (op == "dftu.series.dt_as_unit")
        return "a value does not fit 64 bits in the finer unit, or the column "
               "is not a Timestamp or Duration";
    return nullptr;
}

}  // namespace dftracer::utils::dataframe

#endif
