// SPDX-License-Identifier: MIT
#include <dftracer/utils/coro/async_mutex.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/store/directory_key.h>
#include <dftracer/utils/trace/index/build/index_write_lock.h>
#include <dftracer/utils/trace/index/store/root_layout.h>

#include <mutex>

namespace dftracer::utils::trace::index::build {

namespace {

struct IndexLocks {
    std::mutex mutex;
    dstd::Optional<dstd::StringViewMap<dstd::UniquePtr<coro::AsyncMutex>>>
        by_path;
};

IndexLocks& locks() {
    IndexLocks& held = dstd::Singleton<IndexLocks>::get();
    [[maybe_unused]] static const bool hooked = [] {
        dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches, []() noexcept {
            IndexLocks& dropped = dstd::Singleton<IndexLocks>::get();
            const std::lock_guard<std::mutex> lock(dropped.mutex);
            dropped.by_path.reset();
        });
        return true;
    }();
    return held;
}

}  // namespace

dstd::String index_write_key(dstd::StringView index_path) {
    return dstd::String(
        store::directory_key(trace::index::store_dir(index_path)).str());
}

coro::AsyncMutex& index_write_mutex(dstd::StringView key) {
    IndexLocks& held = locks();
    std::lock_guard<std::mutex> lk(held.mutex);
    if (!held.by_path) held.by_path.emplace();
    auto& slot = (*held.by_path)[key];
    if (!slot) slot = dstd::make_unique<coro::AsyncMutex>();
    return *slot;
}

}  // namespace dftracer::utils::trace::index::build
