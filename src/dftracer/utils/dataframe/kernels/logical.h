// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_KERNELS_LOGICAL_H
#define DFTRACER_UTILS_DATAFRAME_KERNELS_LOGICAL_H

#include <dftracer/utils/dataframe/dataframe.h>

namespace dftracer::utils::dataframe {

/// Elementwise boolean logic on bit-packed Bool columns, returning a Bool
/// column. `logical_and`/`logical_or` need equal-length inputs. Invalid (empty)
/// for non-Bool inputs or a length mismatch.
Series logical_and(const Series& a, const Series& b);
Series logical_or(const Series& a, const Series& b);
Series logical_not(const Series& a);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_KERNELS_LOGICAL_H
