#ifndef DFTRACER_UTILS_INDEX_BUILD_INDEX_WRITE_LOCK_H
#define DFTRACER_UTILS_INDEX_BUILD_INDEX_WRITE_LOCK_H

#include <mutex>
#include <string>

namespace dftracer::utils::index::build {

/// Serializes index writers per index path. Every lazy writer that rides along
/// a scan (dictionary, bloom) takes this first, so their writes of one index
/// commit one at a time.
std::mutex& index_write_mutex(const std::string& index_path);

}  // namespace dftracer::utils::index::build

#endif  // DFTRACER_UTILS_INDEX_BUILD_INDEX_WRITE_LOCK_H
