// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_FILEIO_PARALLEL_MERGE_H
#define DFTRACER_UTILS_FILEIO_PARALLEL_MERGE_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

namespace dftracer::utils::fileio::parallel {

/// Concatenate `shards` into `target` (truncating) and unlink the shards on
/// success. Valid for any format whose bytes concatenate cleanly (plain
/// JSON/NDJSON, gzip members). Shards are left in place on failure.
/// Returns 0 on success, -1 on any I/O failure.
coro::CoroTask<int> merge_shards(dstd::CStringView target,
                                 const dstd::Vector<dstd::String>& shards);

}  // namespace dftracer::utils::fileio::parallel

#endif  // DFTRACER_UTILS_FILEIO_PARALLEL_MERGE_H
