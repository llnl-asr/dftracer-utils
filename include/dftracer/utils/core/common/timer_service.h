#ifndef DFTRACER_UTILS_CORE_COMMON_TIMER_SERVICE_H
#define DFTRACER_UTILS_CORE_COMMON_TIMER_SERVICE_H

#include <concurrentqueue.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

namespace dftracer::utils {

class TimerService {
   public:
    using Callback = std::function<void()>;
    using TimerId = uint64_t;

   private:
    struct Timer {
        TimerId id;
        std::chrono::steady_clock::time_point expiry;
        Callback callback;
        std::shared_ptr<std::atomic<bool>> cancelled;

        bool operator>(const Timer& other) const {
            return expiry > other.expiry;
        }
    };

    struct TimerRequest {
        TimerId id;
        std::chrono::steady_clock::time_point expiry;
        Callback callback;
        std::shared_ptr<std::atomic<bool>> cancelled;
    };

    std::priority_queue<Timer, std::vector<Timer>, std::greater<Timer>>
        timer_heap_;
    moodycamel::ConcurrentQueue<TimerRequest> pending_timers_;
    std::unordered_map<TimerId, std::shared_ptr<std::atomic<bool>>>
        cancellation_tokens_;
    mutable std::mutex tokens_mutex_;  // Protects cancellation_tokens_
    std::thread timer_thread_;
    std::atomic<bool> running_{false};
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
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) {
            return;
        }

        timer_thread_ = std::thread([this]() { timer_loop(); });
    }

    void stop() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return;
        }

        if (timer_thread_.joinable()) {
            timer_thread_.join();
        }
    }

    template <typename Duration>
    TimerId register_timeout(Duration duration, Callback callback) {
        auto expiry = std::chrono::steady_clock::now() + duration;
        TimerId id = next_id_.fetch_add(1, std::memory_order_relaxed);

        auto cancel_token = std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard<std::mutex> lock(tokens_mutex_);
            cancellation_tokens_[id] = cancel_token;
        }

        pending_timers_.enqueue(
            TimerRequest{id, expiry, std::move(callback), cancel_token});
        has_new_timers_.store(true, std::memory_order_release);

        return id;
    }

    void cancel_timeout(TimerId id) {
        std::lock_guard<std::mutex> lock(tokens_mutex_);
        auto it = cancellation_tokens_.find(id);
        if (it != cancellation_tokens_.end()) {
            it->second->store(true, std::memory_order_release);
        }
    }

   private:
    void timer_loop() {
        while (running_.load(std::memory_order_acquire)) {
            process_pending_timers();

            auto now = std::chrono::steady_clock::now();

            if (timer_heap_.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            const Timer& next_timer = timer_heap_.top();

            if (next_timer.expiry <= now) {
                Timer timer = timer_heap_.top();
                timer_heap_.pop();

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
                auto sleep_duration = next_timer.expiry - now;
                if (sleep_duration > std::chrono::milliseconds(10)) {
                    sleep_duration = std::chrono::milliseconds(10);
                }
                std::this_thread::sleep_for(sleep_duration);
            }
        }
    }

    void process_pending_timers() {
        if (!has_new_timers_.load(std::memory_order_acquire)) {
            return;
        }

        TimerRequest req;
        bool dequeued_any = false;
        while (pending_timers_.try_dequeue(req)) {
            timer_heap_.push(Timer{req.id, req.expiry, std::move(req.callback),
                                   req.cancelled});
            dequeued_any = true;
        }

        if (dequeued_any) {
            has_new_timers_.store(false, std::memory_order_release);
        }
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_TIMER_SERVICE_H
