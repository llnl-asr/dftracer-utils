#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_PARALLEL_MERGE_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_PARALLEL_MERGE_H

#include <dftracer/utils/core/coro/task.h>

#include <string>
#include <vector>

namespace dftracer::utils::utilities::fileio::parallel {

/// Concatenate `shards` into `target` (truncating) and unlink the shards on
/// success. Valid for any format whose bytes concatenate cleanly (plain
/// JSON/NDJSON, gzip members). Shards are left in place on failure.
/// Returns 0 on success, -1 on any I/O failure.
coro::CoroTask<int> merge_shards(const std::string& target,
                                 const std::vector<std::string>& shards);

}  // namespace dftracer::utils::utilities::fileio::parallel

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_PARALLEL_MERGE_H
