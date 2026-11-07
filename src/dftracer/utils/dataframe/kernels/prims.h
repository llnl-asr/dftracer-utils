// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_KERNELS_PRIMS_H
#define DFTRACER_UTILS_DATAFRAME_KERNELS_PRIMS_H

#include <dftracer/utils/dataframe/dataframe.h>

namespace dftracer::utils::dataframe {

/// Unary numeric primitives over a FLAT Int64/Uint64 column (the same ops as
/// plugins/prims.h), producing an Int64 column. Cast the input to a 64-bit
/// integer first. `Mix64` yields the hash's bit pattern as int64.
enum class Prim {
    Ilog2 = 0,
    BitWidth = 1,
    Popcount = 2,
    Clz = 3,
    Ctz = 4,
    Mix64 = 5,
};

Series prim(const Series& a, Prim op);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_KERNELS_PRIMS_H
