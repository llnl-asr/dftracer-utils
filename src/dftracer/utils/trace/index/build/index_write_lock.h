// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_BUILD_INDEX_WRITE_LOCK_H
#define DFTRACER_UTILS_TRACE_INDEX_BUILD_INDEX_WRITE_LOCK_H

#include <dftracer/utils/dstd/string.h>

#include <mutex>

namespace dftracer::utils::trace::index::build {

/// Serializes index writers per index path. Every lazy writer that rides along
/// a scan takes this first.
std::mutex& index_write_mutex(const dstd::String& index_path);

}  // namespace dftracer::utils::trace::index::build

#endif  // DFTRACER_UTILS_TRACE_INDEX_BUILD_INDEX_WRITE_LOCK_H
