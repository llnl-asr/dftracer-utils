// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_VALIDATE_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_VALIDATE_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>

#include <cstdint>

struct dftu_series;

namespace dftracer::utils::dataframe {

/// Whether DFTRACER_UTILS_PLUGIN_CHECK is on: the environment is read once, on
/// the first call, and the answer is kept for the life of the process.
bool plugin_check_enabled();

/// Check one column against the encoding it claims: the length, the validity
/// bitmap and its null count, and the bytes its offsets, views, codes, chunks
/// and children need. An InvalidArgument error names the column and the rule
/// that broke.
dstd::Result<void> validate(const dftu_series& col);

/// @ref validate for an owned @ref Series; the error names its column.
dstd::Result<void> validate(const Series& col);

/// Check every column and the frame's names: one name per column, no name
/// twice, every column the same length. The error names the column and the
/// rule.
dstd::Result<void> validate(const DataFrame& frame);

/// The checks above, skipped whole while @ref plugin_check_enabled is false.
dstd::Result<void> validate_if_checking(const dftu_series& col);

dstd::Result<void> validate_if_checking(const Series& col);

dstd::Result<void> validate_if_checking(const DataFrame& frame);

/// The rule the last @ref dftu_op_run or @ref dftu_op_run_frame refused a
/// plugin's result for, or empty.
///
/// Those runners report every failure as a null result, which carries no
/// message, so the check leaves its reason here for the caller to put in its
/// own error. Written only while @ref plugin_check_enabled is true, and cleared
/// by the next run, so a reason never outlives the call that produced it.
/// Thread local: two threads run ops and fail independently.
dstd::StringView op_check_error();

/// Sets @ref op_check_error; an empty @p why clears it.
void record_op_check_error(dstd::StringView why);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_VALIDATE_H
