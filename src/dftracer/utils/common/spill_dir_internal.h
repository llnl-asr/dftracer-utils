// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMMON_SPILL_DIR_INTERNAL_H
#define DFTRACER_UTILS_COMMON_SPILL_DIR_INTERNAL_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/vector.h>

namespace dftracer::utils::common::detail {

/// spill_dir() with its inputs given: `env` is DFTRACER_UTILS_SPILL_DIR (empty
/// when unset), `local_root` is local_spill_root(), and `fallbacks` are tried
/// in order when both are empty. A fallback on a RAM-backed file system is
/// never used; a network one is.
dstd::Result<dstd::Path> choose_spill_dir(
    dstd::StringView env, const dstd::Path& local_root,
    const dstd::Vector<dstd::Path>& fallbacks);

}  // namespace dftracer::utils::common::detail

#endif  // DFTRACER_UTILS_COMMON_SPILL_DIR_INTERNAL_H
