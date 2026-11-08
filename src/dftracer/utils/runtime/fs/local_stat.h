// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_FS_LOCAL_STAT_H
#define DFTRACER_UTILS_RUNTIME_FS_LOCAL_STAT_H
#ifdef __linux__

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>

namespace dftracer::utils::runtime::fs {

/// @ref dstd::fs::stat, or @ref dstd::fs::lstat when @p follow_symlinks is
/// false, through @ref io::statx, so it fills the same fields and gives the
/// same errors. @p path must stay valid until the task ends.
coro::CoroTask<dstd::Expected<dstd::fs::FileStat, dstd::Error>> local_stat(
    dstd::PathView path, bool follow_symlinks);

}  // namespace dftracer::utils::runtime::fs

#endif  // __linux__
#endif  // DFTRACER_UTILS_RUNTIME_FS_LOCAL_STAT_H
