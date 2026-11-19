// SPDX-License-Identifier: MIT
#include <dftracer/utils/runtime/io/io_completion_thread.h>

namespace dftracer::utils::runtime::io {

IoCompletionThread::~IoCompletionThread() {
    if (running_.load(std::memory_order_relaxed)) {
        stop();
    }
}

dstd::Result<void> IoCompletionThread::start(dstd::Function<void()> poll_fn) {
    poll_fn_ = std::move(poll_fn);
    running_.store(true, std::memory_order_release);
    auto thread = dstd::Thread::try_start([this] {
        while (running_.load(std::memory_order_acquire)) {
            poll_fn_();
        }
    });
    if (!thread) {
        running_.store(false, std::memory_order_release);
        return dstd::Unexpected(thread.error());
    }
    thread_ = std::move(*thread);
    return {};
}

void IoCompletionThread::signal_stop() {
    running_.store(false, std::memory_order_release);
}

void IoCompletionThread::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void IoCompletionThread::stop() {
    signal_stop();
    join();
}

}  // namespace dftracer::utils::runtime::io
