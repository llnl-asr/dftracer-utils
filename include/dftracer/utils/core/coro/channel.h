#ifndef DFTRACER_UTILS_CORE_CORO_CHANNEL_H
#define DFTRACER_UTILS_CORE_CORO_CHANNEL_H

#include <blockingconcurrentqueue.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>

namespace dftracer::utils::coro {

/**
 * Channel<T> - Producer-consumer queue for streaming data
 *
 * Usage:
 * @code
 * Channel<Chunk> channel(1000);
 *
 * // Producer task
 * auto producer = make_task([&](TaskContext& ctx) -> CoroTask<void> {
 *     Channel<Chunk>::ProducerGuard guard(&channel);
 *     for (auto chunk : read_chunks()) {
 *         channel.send_blocking(std::move(chunk));
 *     }
 *     // ProducerGuard destructor auto-closes when last producer exits
 * });
 *
 * // Consumer task
 * auto consumer = make_task([&](TaskContext& ctx) -> CoroTask<void> {
 *     Chunk chunk;
 *     while (channel.receive(chunk)) {
 *         process(chunk);
 *     }
 * });
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
       private:
        Channel* channel_;

       public:
        explicit ProducerGuard(Channel* ch) : channel_(ch) {
            if (channel_) {
                channel_->num_producers_.fetch_add(1,
                                                   std::memory_order_relaxed);
            }
        }

        ~ProducerGuard() {
            if (!channel_) return;
            std::size_t prev = channel_->num_producers_.fetch_sub(
                1, std::memory_order_acq_rel);
            if (prev == 1) {
                // Wait until no in-flight sends remain
                while (channel_->active_sends_.load(
                           std::memory_order_acquire) != 0) {
                    std::this_thread::yield();
                }
                // Mark closed for consumers only if user hasn't closed already
                if (!channel_->user_closed_.load(std::memory_order_acquire))
                    channel_->closed_.store(true, std::memory_order_release);
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
                        std::atomic_thread_fence(std::memory_order_seq_cst);
                        channel_->close();
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
    std::atomic<std::size_t> num_producers_{0};
    std::atomic<std::size_t> active_sends_{0};
    std::atomic<bool> user_closed_{false};
    std::atomic<bool> closed_{false};

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
     * Pre-register a producer before the task actually starts
     */
    void register_producer() {
        num_producers_.fetch_add(1, std::memory_order_release);
    }

    /**
     * Release a pre-registered producer slot
     */
    void release_producer() {
        std::size_t prev =
            num_producers_.fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1) {
            while (active_sends_.load(std::memory_order_acquire) != 0) {
                std::this_thread::yield();
            }
            if (!user_closed_.load(std::memory_order_acquire)) {
                closed_.store(true, std::memory_order_release);
            }
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
        while (capacity_ != SIZE_MAX && queue_.size_approx() >= capacity_) {
            if (user_closed_.load(std::memory_order_acquire)) {
                active_sends_.fetch_sub(1, std::memory_order_acq_rel);
                return false;
            }
            std::this_thread::yield();
        }

        queue_.enqueue(std::move(item));
        active_sends_.fetch_sub(1, std::memory_order_acq_rel);
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
            if (queue_.try_dequeue(item)) return true;

            // If there are still producers, wait a bit for data
            if (num_producers_.load(std::memory_order_acquire) > 0) {
                if (queue_.wait_dequeue_timed(item,
                                              std::chrono::milliseconds(1)))
                    return true;
                continue;
            }

            // No producers remain: final drain until we’re sure it’s empty.
            // Try a few fast attempts and one short timed wait.
            for (int i = 0; i < 128; ++i) {
                if (queue_.try_dequeue(item)) return true;
                std::this_thread::yield();
            }
            if (queue_.wait_dequeue_timed(item, std::chrono::microseconds(200)))
                return true;

            // Double-check once more; if still nothing, we’re done.
            if (!queue_.try_dequeue(item)) return false;
            return true;
        }
    }

    /**
     * Try to receive without blocking
     *
     * @param item Output parameter for received item
     * @return true if received, false if queue empty
     */
    bool try_receive(T& item) { return queue_.try_dequeue(item); }

    /**
     * Close channel
     * No more items can be sent after this
     */
    void close() {
        user_closed_.store(true, std::memory_order_release);
        closed_.store(true, std::memory_order_release);
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
