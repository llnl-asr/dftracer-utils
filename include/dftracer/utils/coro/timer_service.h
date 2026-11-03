// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_TIMER_SERVICE_H
#define DFTRACER_UTILS_CORO_TIMER_SERVICE_H

#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/queue.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vendor/concurrentqueue.h>
#include <dftracer/utils/memory/manager.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>

namespace dftracer::utils::coro {

class TimerService {
   public:
    using Callback = dstd::Function<void()>;
    using TimerId = uint64_t;

    /// The id of a timeout that was not registered, so nothing fires it.
    static constexpr TimerId NO_TIMER = 0;

   private:
    struct Timer {
        TimerId id;
        std::chrono::steady_clock::time_point expiry;
        Callback callback;
        dstd::SharedPtr<std::atomic<bool>> cancelled;

        bool operator>(const Timer& other) const {
            return expiry > other.expiry;
        }
    };

    struct TimerRequest {
        TimerId id;
        std::chrono::steady_clock::time_point expiry;
        Callback callback;
        dstd::SharedPtr<std::atomic<bool>> cancelled;
    };

    dstd::PriorityQueue<Timer, std::greater<Timer>> timer_heap_;
    moodycamel::ConcurrentQueue<TimerRequest> pending_timers_;
    dstd::Map<TimerId, dstd::SharedPtr<std::atomic<bool>>> cancellation_tokens_;
    mutable std::mutex tokens_mutex_;  ///< Protects cancellation_tokens_
    dstd::Thread timer_thread_;
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    std::atomic<bool> running_{false};
    std::atomic<bool> retired_{false};
    std::atomic<bool> has_new_timers_{false};
    std::atomic<TimerId> next_id_{1};

   public:
    TimerService() = default;
    ~TimerService() { stop(); }

    TimerService(const TimerService&) = delete;
    TimerService& operator=(const TimerService&) = delete;
    TimerService(TimerService&&) = delete;
    TimerService& operator=(TimerService&&) = delete;

    void start() {
        if (retired_.load(std::memory_order_acquire)) return;
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            return;
        }

        timer_thread_ = dstd::Thread([this]() { timer_loop(); });
    }

    void stop() {
        bool expected = true;
        {
            std::lock_guard<std::mutex> lock(wake_mutex_);
            if (!running_.compare_exchange_strong(expected, false)) {
                return;
            }
        }
        wake_cv_.notify_all();

        if (timer_thread_.joinable()) {
            timer_thread_.join();
        }

        // Timers left over would never fire, and their memory is counted.
        timer_heap_ = decltype(timer_heap_){};
        TimerRequest req;
        while (pending_timers_.try_dequeue(req)) {
        }
        std::lock_guard<std::mutex> lock(tokens_mutex_);
        cancellation_tokens_.clear();
    }

    /// Stops the timer thread for good. A later start() does nothing.
    void retire() {
        retired_.store(true, std::memory_order_release);
        stop();
    }

    /// Whether the timer thread is running.
    bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    template <typename Duration>
    TimerId register_timeout(Duration duration, Callback callback) {
        // Nothing would fire it, so the caller waits without a timeout.
        if (retired_.load(std::memory_order_acquire)) return NO_TIMER;
        auto expiry = std::chrono::steady_clock::now() + duration;
        TimerId id = next_id_.fetch_add(1, std::memory_order_relaxed);

        // On the general pool: the timer keeps these past the caller's pool.
        memory::Scope scope{memory::Manager::get().general()};
        auto cancel_token = dstd::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard<std::mutex> lock(tokens_mutex_);
            cancellation_tokens_[id] = cancel_token;
        }

        pending_timers_.enqueue(
            TimerRequest{id, expiry, std::move(callback), cancel_token});
        {
            std::lock_guard<std::mutex> lock(wake_mutex_);
            has_new_timers_.store(true, std::memory_order_release);
        }
        wake_cv_.notify_one();

        return id;
    }

    void cancel_timeout(TimerId id) {
        std::lock_guard<std::mutex> lock(tokens_mutex_);
        auto it = cancellation_tokens_.find(id);
        if (it != cancellation_tokens_.end()) {
            it->second->store(true, std::memory_order_release);
        }
        wake_cv_.notify_one();
    }

   private:
    void timer_loop() {
        while (running_.load(std::memory_order_acquire)) {
            process_pending_timers();

            auto now = std::chrono::steady_clock::now();

            if (timer_heap_.empty()) {
                std::unique_lock<std::mutex> lock(wake_mutex_);
                wake_cv_.wait(lock, [this] {
                    return !running_.load(std::memory_order_acquire) ||
                           has_new_timers_.load(std::memory_order_acquire);
                });
                continue;
            }

            const Timer& next_timer = timer_heap_.top();

            if (next_timer.expiry <= now) {
                Timer timer = std::move(const_cast<Timer&>(timer_heap_.top()));
                timer_heap_.pop();
                // An idle service holds no counted memory. Each timer already
                // allocates its token, so one more allocation is small.
                if (timer_heap_.empty()) timer_heap_ = decltype(timer_heap_){};

                if (!timer.cancelled->load(std::memory_order_acquire) &&
                    timer.callback) {
                    timer.callback();
                }

                {
                    std::lock_guard<std::mutex> lock(tokens_mutex_);
                    auto it = cancellation_tokens_.find(timer.id);
                    if (it != cancellation_tokens_.end()) {
                        cancellation_tokens_.erase(it);
                    }
                }
            } else {
                std::unique_lock<std::mutex> lock(wake_mutex_);
                wake_cv_.wait_until(lock, next_timer.expiry, [this] {
                    return !running_.load(std::memory_order_acquire) ||
                           has_new_timers_.load(std::memory_order_acquire);
                });
            }
        }
    }

    void process_pending_timers() {
        TimerRequest req;
        while (has_new_timers_.exchange(false, std::memory_order_acq_rel)) {
            while (pending_timers_.try_dequeue(req)) {
                timer_heap_.push(Timer{req.id, req.expiry,
                                       std::move(req.callback), req.cancelled});
            }
        }
    }
};

/// The process-wide timers, made and started on first use, and stopped at
/// shutdown after the runtime. An executor that names no timers of its own
/// uses these. They stay stopped once shutdown began, and a timeout registered
/// then returns TimerService::NO_TIMER.
TimerService& process_timers();

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_TIMER_SERVICE_H
