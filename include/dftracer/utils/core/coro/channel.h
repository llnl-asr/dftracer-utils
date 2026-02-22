#ifndef DFTRACER_UTILS_CORE_CORO_CHANNEL_H
#define DFTRACER_UTILS_CORE_CORO_CHANNEL_H

#include <blockingconcurrentqueue.h>

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace dftracer::utils::coro {

/**
 * Channel<T> - Producer-consumer queue for streaming data
 *
 * Usage (simple — guard registers and releases automatically):
 * @code
 * auto channel = make_channel<Chunk>(1000);
 *
 * auto producer = make_task([&](TaskContext& ctx) -> CoroTask<void> {
 *     auto guard = channel->producer_guard();  // registers
 *     for (auto chunk : read_chunks())
 *         channel->send_blocking(std::move(chunk));
 *     // ~ProducerGuard auto-releases; channel closes when last exits
 * });
 * @endcode
 *
 * Usage (coroutines — pre-register then adopt for RAII release):
 * @code
 * auto channel = make_channel<Chunk>(0);
 *
 * // Pre-register before spawning so consumers see producers immediately
 * for (std::size_t i = 0; i < N; ++i)
 *     channel->register_producer();
 *
 * for (std::size_t i = 0; i < N; ++i) {
 *     scope.spawn([&](TaskContext& ctx) -> CoroTask<void> {
 *         auto guard = channel->adopt_producer();  // no increment
 *         // ... work ...
 *         co_return;  // ~ProducerGuard auto-releases
 *     });
 * }
 * @endcode
 */
template <typename T>
class Channel : public std::enable_shared_from_this<Channel<T>> {
   public:
    /**
     * RAII guard for producer tracking
     * Automatically closes channel when last producer exits
     */
    class ProducerGuard {
       public:
        /// Tag type: adopt an already-registered producer slot (no increment).
        struct Adopt {};

       private:
        Channel* channel_;

       public:
        /// Register a new producer slot.
        explicit ProducerGuard(Channel* ch) : channel_(ch) {
            if (channel_) {
                channel_->had_producers_.store(true, std::memory_order_release);
                channel_->num_producers_.fetch_add(1,
                                                   std::memory_order_relaxed);
            }
        }

        /// Adopt an existing producer registration (no increment).
        /// Use after register_producer() when you need RAII release only.
        ProducerGuard(Channel* ch, Adopt) : channel_(ch) {}

        ~ProducerGuard() {
            if (!channel_) return;
            std::size_t prev = channel_->num_producers_.fetch_sub(
                1, std::memory_order_acq_rel);
            if (prev == 1) {
                // Mark closed for consumers only if user hasn't closed already
                if (!channel_->user_closed_.load(std::memory_order_acquire))
                    channel_->closed_.store(true, std::memory_order_release);
                channel_->notify_all_waiters();
            }
        }

        // Non-copyable
        ProducerGuard(const ProducerGuard&) = delete;
        ProducerGuard& operator=(const ProducerGuard&) = delete;

        // Movable
        ProducerGuard(ProducerGuard&& other) noexcept
            : channel_(other.channel_) {
            other.channel_ = nullptr;
        }

        ProducerGuard& operator=(ProducerGuard&& other) noexcept {
            if (this != &other) {
                if (channel_) {
                    // Release current channel
                    std::size_t remaining = channel_->num_producers_.fetch_sub(
                        1, std::memory_order_acq_rel);
                    if (remaining == 1) {
                        if (!channel_->user_closed_.load(
                                std::memory_order_acquire)) {
                            channel_->closed_.store(true,
                                                    std::memory_order_release);
                        }
                        channel_->notify_all_waiters();
                    }
                }
                channel_ = other.channel_;
                other.channel_ = nullptr;
            }
            return *this;
        }
    };

   private:
    moodycamel::BlockingConcurrentQueue<T> queue_;

    std::size_t capacity_;
    mutable std::mutex state_mutex_;
    std::condition_variable cv_readable_;
    std::condition_variable cv_writable_;
    std::atomic<std::size_t> num_producers_{0};
    std::atomic<bool> had_producers_{false};
    std::atomic<std::size_t> active_sends_{0};
    std::atomic<bool> user_closed_{false};
    std::atomic<bool> closed_{false};

    void notify_all_waiters() {
        cv_readable_.notify_all();
        cv_writable_.notify_all();
    }

    void maybe_notify_terminal() {
        if (num_producers_.load(std::memory_order_acquire) == 0 &&
            active_sends_.load(std::memory_order_acquire) == 0) {
            cv_readable_.notify_all();
        }
    }

   public:
    /**
     * Constructor
     * @param capacity Maximum number of items in queue (0 = unlimited)
     */
    explicit Channel(std::size_t capacity = 0)
        : queue_(capacity == 0 ? 1024 : capacity),
          capacity_(capacity == 0 ? SIZE_MAX : capacity) {}

    ~Channel() { close(); }

    // Non-copyable, non-movable (moodycamel queue is non-movable)
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    Channel(Channel&&) = delete;
    Channel& operator=(Channel&&) = delete;

    /**
     * Helper to create shared_ptr Channel<T>
     */
    static std::shared_ptr<Channel<T>> make(std::size_t capacity = 0) {
        return std::make_shared<Channel<T>>(capacity);
    }

    /**
     * Get producer guard (RAII)
     * Use this to automatically close channel when last producer exits
     */
    ProducerGuard producer_guard() { return ProducerGuard(this); }

    /**
     * Adopt an already-registered producer slot as an RAII guard.
     * Call register_producer() first, then adopt_producer() inside the
     * coroutine/thread to get automatic release on scope exit.
     */
    ProducerGuard adopt_producer() {
        return ProducerGuard(this, typename ProducerGuard::Adopt{});
    }

    /**
     * Pre-register a single producer before the task actually starts
     */
    void register_producer() {
        had_producers_.store(true, std::memory_order_release);
        num_producers_.fetch_add(1, std::memory_order_release);
    }

    /**
     * Pre-register multiple producers at once
     */
    void register_producers(std::size_t n) {
        if (n > 0) {
            had_producers_.store(true, std::memory_order_release);
            num_producers_.fetch_add(n, std::memory_order_release);
        }
    }

    /**
     * Release a pre-registered producer slot
     */
    void release_producer() {
        std::size_t prev =
            num_producers_.fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1) {
            if (!user_closed_.load(std::memory_order_acquire)) {
                closed_.store(true, std::memory_order_release);
            }
            notify_all_waiters();
        }
    }

    /**
     * Blocking send (for thread-based producers)
     * Blocks if queue is full until space is available
     *
     * @param item Item to send
     * @return true if sent, false if channel closed
     */
    bool send_blocking(T item) {
        if (user_closed_.load(std::memory_order_acquire)) return false;
        active_sends_.fetch_add(1, std::memory_order_acq_rel);

        // Wait for space if queue is at capacity
        if (capacity_ != SIZE_MAX) {
            std::unique_lock<std::mutex> lock(state_mutex_);
            cv_writable_.wait(lock, [this]() {
                return queue_.size_approx() < capacity_ ||
                       user_closed_.load(std::memory_order_acquire);
            });

            if (user_closed_.load(std::memory_order_acquire)) {
                active_sends_.fetch_sub(1, std::memory_order_acq_rel);
                maybe_notify_terminal();
                return false;
            }
        }

        queue_.enqueue(std::move(item));
        active_sends_.fetch_sub(1, std::memory_order_acq_rel);
        cv_readable_.notify_one();
        maybe_notify_terminal();
        return true;
    }

    /**
     * Try to send without blocking
     *
     * @param item Item to send
     * @return true if sent, false if queue full or closed
     */
    bool try_send(T item) {
        if (user_closed_.load(std::memory_order_acquire)) return false;
        active_sends_.fetch_add(1, std::memory_order_acq_rel);
        bool ok = queue_.try_enqueue(std::move(item));
        active_sends_.fetch_sub(1, std::memory_order_acq_rel);
        if (ok) {
            cv_readable_.notify_one();
        }
        maybe_notify_terminal();
        return ok;
    }

    /**
     * Blocking receive (for thread-based consumers)
     * Blocks if queue is empty until item is available
     *
     * @param item Output parameter for received item
     * @return true if received, false if channel closed and empty
     */
    bool receive(T& item) {
        for (;;) {
            if (queue_.try_dequeue(item)) {
                cv_writable_.notify_one();
                return true;
            }

            std::unique_lock<std::mutex> lock(state_mutex_);

            if (queue_.try_dequeue(item)) {
                lock.unlock();
                cv_writable_.notify_one();
                return true;
            }

            const bool no_producers =
                num_producers_.load(std::memory_order_acquire) == 0;
            const bool had_producers =
                had_producers_.load(std::memory_order_acquire);
            const bool no_active_sends =
                active_sends_.load(std::memory_order_acquire) == 0;
            const bool user_closed =
                user_closed_.load(std::memory_order_acquire);

            if ((user_closed || (had_producers && no_producers)) &&
                no_active_sends) {
                return false;
            }

            cv_readable_.wait(lock);
        }
    }

    /**
     * Try to receive without blocking
     *
     * @param item Output parameter for received item
     * @return true if received, false if queue empty
     */
    bool try_receive(T& item) {
        const bool ok = queue_.try_dequeue(item);
        if (ok) {
            cv_writable_.notify_one();
        }
        return ok;
    }

    /**
     * Close channel
     * No more items can be sent after this
     */
    void close() {
        user_closed_.store(true, std::memory_order_release);
        closed_.store(true, std::memory_order_release);
        notify_all_waiters();
    }

    /**
     * Check if channel is closed
     */
    bool is_closed() const { return closed_.load(std::memory_order_acquire); }

    /**
     * Check if channel is closed and no producers remain
     */
    bool is_closed_and_done() const {
        return is_closed() &&
               num_producers_.load(std::memory_order_acquire) == 0;
    }

    /**
     * Get number of active producers
     */
    std::size_t num_producers() const {
        return num_producers_.load(std::memory_order_acquire);
    }

    /**
     * Get current queue size (approximate, lock-free)
     */
    std::size_t size() const { return queue_.size_approx(); }

    /**
     * Get channel capacity
     */
    std::size_t capacity() const { return capacity_; }

    /**
     * Check if queue is empty (approximate)
     */
    bool empty() const { return queue_.size_approx() == 0; }

    /**
     * Check if queue is full (approximate)
     */
    bool full() const { return queue_.size_approx() >= capacity_; }
};

/**
 * Helper to create shared_ptr Channel<T>
 */
template <typename T>
std::shared_ptr<Channel<T>> make_channel(std::size_t capacity) {
    return Channel<T>::make(capacity);
}
}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORE_CORO_CHANNEL_H
