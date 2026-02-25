#include "io_completion_thread.h"

namespace dftracer::utils::io {

IoCompletionThread::~IoCompletionThread() {
    if (running_.load(std::memory_order_relaxed)) {
        stop();
    }
}

void IoCompletionThread::start(std::function<void()> poll_fn) {
    poll_fn_ = std::move(poll_fn);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] {
        while (running_.load(std::memory_order_acquire)) {
            poll_fn_();
        }
    });
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

}  // namespace dftracer::utils::io
