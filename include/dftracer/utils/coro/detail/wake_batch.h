// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_DETAIL_WAKE_BATCH_H
#define DFTRACER_UTILS_CORO_DETAIL_WAKE_BATCH_H

#include <dftracer/utils/coro/detail/resumption_helper.h>
#include <dftracer/utils/dstd/vector.h>

#include <coroutine>
#include <cstddef>

namespace dftracer::utils::coro::detail {

/// Resumes collected under the channel lock and posted after it. A posted
/// coroutine can run and free its awaitable at once, so this copies out of the
/// waiter nodes and never reads them again.
class WakeBatch {
   public:
    void add(std::coroutine_handle<> handle, Executor* executor) {
        if (count_ < INLINE_WAKES) {
            inline_[count_++] = {handle, executor};
        } else {
            overflow_.push_back({handle, executor});
        }
    }

    void post() {
        for (std::size_t i = 0; i < count_; ++i) {
            post_resumption(inline_[i].executor, inline_[i].handle);
        }
        for (const Wake& wake : overflow_) {
            post_resumption(wake.executor, wake.handle);
        }
    }

   private:
    struct Wake {
        std::coroutine_handle<> handle;
        Executor* executor{nullptr};
    };

    static constexpr std::size_t INLINE_WAKES = 16;

    Wake inline_[INLINE_WAKES];
    std::size_t count_{0};
    dstd::Vector<Wake> overflow_;
};

}  // namespace dftracer::utils::coro::detail

#endif  // DFTRACER_UTILS_CORO_DETAIL_WAKE_BATCH_H
