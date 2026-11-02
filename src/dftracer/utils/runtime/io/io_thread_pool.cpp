// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/runtime/io/io_thread_pool.h>

#include <algorithm>
#include <chrono>
#include <system_error>

namespace dftracer::utils::io {

namespace {
// How long an I/O thread waits for work before it gives back its idle pages.
constexpr std::chrono::milliseconds IDLE_TRIM_AFTER{50};
}  // namespace

IoThreadPool::IoThreadPool(std::size_t num_threads, unsigned batch_threshold,
                           IoPoolOptions options)
    : num_threads_(num_threads == 0 ? 4 : num_threads),
      batch_threshold_(batch_threshold),
      keepalive_(options.keepalive),
      eager_(options.eager) {}

IoThreadPool::~IoThreadPool() { stop(); }

void IoThreadPool::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load()) return;
    cancelling_.store(false, std::memory_order_release);
    running_ = true;
    const std::size_t count =
        eager_ ? num_threads_.load(std::memory_order_acquire) : 1;
    threads_.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        if (!spawn_locked()) break;
}

void IoThreadPool::grow(std::size_t threads, bool eager) {
    // Under the lock stop() takes to clear running_, so a pool that stops
    // either joins the new threads or never gets them.
    std::lock_guard<std::mutex> lock(mutex_);
    eager_ = eager;
    if (threads > num_threads_.load(std::memory_order_acquire))
        num_threads_.store(threads, std::memory_order_release);
    if (!eager_ || !running_.load()) return;
    const std::size_t cap = num_threads_.load(std::memory_order_acquire);
    while (live_.load(std::memory_order_relaxed) < cap)
        if (!spawn_locked()) break;
}

bool IoThreadPool::spawn_locked() {
    reap_locked();
    try {
        threads_.emplace_back(&IoThreadPool::worker_loop, this);
    } catch (const std::system_error&) {
        // A pool with a thread still runs the queue, only slower.
        if (live_.load(std::memory_order_relaxed) == 0) throw;
        if (!spawn_warned_) {
            spawn_warned_ = true;
            DFTRACER_UTILS_LOG_WARN(
                "I/O thread pool: cannot start a thread: %s",
                "the pool keeps the threads it has");
        }
        return false;
    }
    ++idle_;
    live_.fetch_add(1, std::memory_order_release);
    return true;
}

void IoThreadPool::reap_locked() {
    for (const std::thread::id id : retired_) {
        for (std::size_t i = 0; i < threads_.size(); ++i) {
            if (threads_[i].get_id() != id) continue;
            threads_[i].join();
            threads_.erase(threads_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    retired_.clear();
}

void IoThreadPool::stop() {
    dstd::Vector<std::thread> joining;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load()) return;
        running_ = false;
        joining.swap(threads_);
        retired_.clear();
    }
    cv_.notify_all();
    for (auto& t : joining) {
        if (t.joinable()) t.join();
    }
}

void IoThreadPool::submit(std::function<void()> fn) {
    std::size_t waking = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(fn));
        if (!eager_ && running_.load(std::memory_order_relaxed) &&
            queue_.size() > idle_ &&
            live_.load(std::memory_order_relaxed) <
                num_threads_.load(std::memory_order_relaxed))
            spawn_locked();
        const unsigned unflushed =
            unflushed_count_.load(std::memory_order_relaxed) + 1;
        if (unflushed >= batch_threshold_) {
            unflushed_count_.store(0, std::memory_order_relaxed);
            waking = unflushed;
        } else {
            unflushed_count_.store(unflushed, std::memory_order_relaxed);
        }
    }
    if (waking != 0) wake(waking);
}

std::size_t IoThreadPool::flush() {
    if (unflushed_count_.load(std::memory_order_relaxed) == 0) return 0;
    std::size_t count;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        count = unflushed_count_.load(std::memory_order_relaxed);
        unflushed_count_.store(0, std::memory_order_relaxed);
    }
    wake(count);
    return count;
}

void IoThreadPool::wake(std::size_t items) {
    // A woken worker drains the queue, so waking the whole pool per flush just
    // thunders it; wake only as many workers as pending items.
    const std::size_t pool = num_threads_.load(std::memory_order_relaxed);
    const std::size_t n = items < pool ? items : pool;
    for (std::size_t i = 0; i < n; ++i) {
        cv_.notify_one();
    }
}

void IoThreadPool::worker_loop() {
    // A thread counts as idle from its spawn until it takes its first job.
    bool counted_idle = true;
    while (true) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!counted_idle) ++idle_;
            counted_idle = false;
            const auto ready = [this] {
                return !running_.load() || !queue_.empty();
            };
            const auto trim_after = std::min(IDLE_TRIM_AFTER, keepalive_);
            if (!ready() && !cv_.wait_for(lock, trim_after, ready)) {
                // Only this thread may purge its heap.
                lock.unlock();
                memory::trim_thread();
                lock.lock();
            }
            const auto above_floor = [this] {
                return live_.load(std::memory_order_relaxed) >
                       (eager_ ? num_threads_.load(std::memory_order_relaxed)
                               : std::size_t{1});
            };
            while (!ready()) {
                if (!above_floor()) {
                    cv_.wait(lock, ready);
                } else if (!cv_.wait_for(lock, keepalive_, ready) &&
                           above_floor()) {
                    // Deciding to leave and seeing an empty queue happen under
                    // one lock hold. A submit either queued before it and keeps
                    // this thread, or sees idle_ without this thread and
                    // spawns.
                    --idle_;
                    live_.fetch_sub(1, std::memory_order_release);
                    retired_.push_back(std::this_thread::get_id());
                    retirements_.fetch_add(1, std::memory_order_release);
                    return;
                }
            }
            if (queue_.empty()) {
                --idle_;
                live_.fetch_sub(1, std::memory_order_release);
                return;
            }
            fn = std::move(queue_.front());
            queue_.pop();
            --idle_;
        }
        fn();
    }
}

}  // namespace dftracer::utils::io
