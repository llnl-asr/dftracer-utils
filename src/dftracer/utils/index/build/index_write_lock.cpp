// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/index/build/index_write_lock.h>

namespace dftracer::utils::index::build {

namespace {

struct IndexLocks {
    std::mutex mutex;
    dstd::StringViewMap<dstd::UniquePtr<std::mutex>> by_path;
};

}  // namespace

std::mutex& index_write_mutex(const dstd::String& index_path) {
    IndexLocks& locks = dstd::Singleton<IndexLocks>::get();
    std::lock_guard<std::mutex> lk(locks.mutex);
    auto& slot = locks.by_path[index_path];
    if (!slot) slot = dstd::make_unique<std::mutex>();
    return *slot;
}

}  // namespace dftracer::utils::index::build
