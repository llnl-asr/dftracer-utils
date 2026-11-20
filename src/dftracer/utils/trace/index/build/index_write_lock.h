// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_BUILD_INDEX_WRITE_LOCK_H
#define DFTRACER_UTILS_TRACE_INDEX_BUILD_INDEX_WRITE_LOCK_H

#include <dftracer/utils/coro/async_mutex.h>
#include <dftracer/utils/dstd/string.h>

namespace dftracer::utils::trace::index::build {

/// The key of the lock of index root @p index_path: the key the store gives its
/// directory, so every spelling of one root has one key.
dstd::String index_write_key(dstd::StringView index_path);

/// Serializes the builds of one index: the store takes one builder per file at
/// a time, so a build that starts while another runs on the same index waits
/// for it. @p key is an @ref index_write_key. A build locks every index it
/// writes, in key order.
coro::AsyncMutex& index_write_mutex(dstd::StringView key);

}  // namespace dftracer::utils::trace::index::build

#endif  // DFTRACER_UTILS_TRACE_INDEX_BUILD_INDEX_WRITE_LOCK_H
