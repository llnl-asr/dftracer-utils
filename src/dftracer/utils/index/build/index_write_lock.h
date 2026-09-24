#ifndef DFTRACER_UTILS_INDEX_BUILD_INDEX_WRITE_LOCK_H
#define DFTRACER_UTILS_INDEX_BUILD_INDEX_WRITE_LOCK_H

#include <mutex>
#include <string>

namespace dftracer::utils::index::build {

/// Serializes index writers per index path. The contention is the open, not the
/// write: each committer opens its own short-lived writable handle (rather than
/// caching one, which would hold the lock for the process lifetime and block
/// the indexer sharing the process), and RocksDB allows only one writable open
/// per directory via its LOCK file. Every lazy writer that rides along a scan
/// (dictionary, bloom) takes this first so their opens queue instead of one
/// failing with a lock error.
std::mutex& index_write_mutex(const std::string& index_path);

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_INDEX_WRITE_LOCK_H
