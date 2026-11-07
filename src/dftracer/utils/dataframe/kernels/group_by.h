// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_KERNELS_GROUP_BY_H
#define DFTRACER_UTILS_DATAFRAME_KERNELS_GROUP_BY_H

#include <dftracer/utils/dataframe/dataframe.h>

#include <cstdint>

namespace dftracer::utils::dataframe {

/// Group `values` by `keys` (equal length) in one pass. `ops` is a bitwise-or
/// of DFTU_AGG_* flags. Returns a DataFrame whose first column is "key" (the
/// distinct keys in first-seen order) followed by one column per requested
/// aggregate, named "sum"/"min"/"max"/"count"/"mean". Nulls in `values` are
/// skipped. The key column may be numeric, String/Binary, or dictionary-
/// encoded; the value column must be numeric.
DataFrame group_by(const Series& keys, const Series& values, std::int32_t ops);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_KERNELS_GROUP_BY_H
