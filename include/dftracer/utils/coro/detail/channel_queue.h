// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_DETAIL_CHANNEL_QUEUE_H
#define DFTRACER_UTILS_CORO_DETAIL_CHANNEL_QUEUE_H

#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/dstd/concurrent_queue.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/vector.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <thread>
#include <utility>

namespace dftracer::utils::coro::detail {

enum class PushStatus { Ok, Full, Closed };
enum class PopStatus { Ok, Empty, Closed };

/// Logs and aborts. An empty ring is a bug in the caller, and the unbounded
/// flavor has its own type.
[[noreturn]] void abort_zero_channel_capacity();

/// Bounded many-to-many ring. Algorithm and the mark bit from the array flavor
/// of crossbeam-channel, https://github.com/crossbeam-rs/crossbeam
/// (MIT or Apache-2.0), itself after Vyukov's bounded MPMC queue.
///
/// The head and tail words hold an index in the low bits, then the mark bit,
/// then a lap count. Each slot stamp says which lap may use it next. The mark
/// bit lives in the tail, so a push and a close are ordered: no push succeeds
/// after a close returns.
template <typename T>
class RingQueue {
   public:
    using value_type = T;

    explicit RingQueue(std::size_t capacity)
        : capacity_(capacity),
          mark_bit_(next_power_of_two(capacity + 1)),
          one_lap_(mark_bit_ * 2),
          slots_(capacity) {
        if (capacity == 0) abort_zero_channel_capacity();
        for (std::size_t i = 0; i < capacity_; ++i) {
            slots_[i].stamp.store(i, std::memory_order_relaxed);
        }
    }

    RingQueue(const RingQueue&) = delete;
    RingQueue& operator=(const RingQueue&) = delete;

    ~RingQueue() {
        dstd::Optional<T> item;
        while (try_pop(item) == PopStatus::Ok) item.reset();
    }

    /// Moves from `item` only when it returns Ok.
    PushStatus try_push(T& item) {
        std::size_t tail = tail_.load(std::memory_order_relaxed);
        int spins = 0;
        for (;;) {
            if ((tail & mark_bit_) != 0) return PushStatus::Closed;
            const std::size_t index = tail & (mark_bit_ - 1);
            const std::size_t lap = tail & ~(one_lap_ - 1);
            Slot& slot = slots_[index];
            const std::size_t stamp =
                slot.stamp.load(std::memory_order_acquire);
            if (stamp == tail) {
                const std::size_t next =
                    index + 1 < capacity_ ? tail + 1 : lap + one_lap_;
                if (tail_.compare_exchange_weak(tail, next,
                                                std::memory_order_seq_cst,
                                                std::memory_order_relaxed)) {
                    ::new (static_cast<void*>(slot.storage)) T(std::move(item));
                    slot.stamp.store(tail + 1, std::memory_order_release);
                    return PushStatus::Ok;
                }
            } else if (stamp + one_lap_ == tail + 1) {
                const std::size_t head = head_.load(std::memory_order_seq_cst);
                if (head + one_lap_ == tail) return PushStatus::Full;
                backoff(spins);
                tail = tail_.load(std::memory_order_relaxed);
            } else {
                backoff(spins);
                tail = tail_.load(std::memory_order_relaxed);
            }
        }
    }

    /// Closed means marked and drained. A push that won its slot before the
    /// mark is still seen, because the pop waits for its stamp.
    PopStatus try_pop(dstd::Optional<T>& out) {
        std::size_t head = head_.load(std::memory_order_relaxed);
        int spins = 0;
        for (;;) {
            const std::size_t index = head & (mark_bit_ - 1);
            const std::size_t lap = head & ~(one_lap_ - 1);
            Slot& slot = slots_[index];
            const std::size_t stamp =
                slot.stamp.load(std::memory_order_acquire);
            if (stamp == head + 1) {
                const std::size_t next =
                    index + 1 < capacity_ ? head + 1 : lap + one_lap_;
                if (head_.compare_exchange_weak(head, next,
                                                std::memory_order_seq_cst,
                                                std::memory_order_relaxed)) {
                    T* value = std::launder(reinterpret_cast<T*>(slot.storage));
                    out.emplace(std::move(*value));
                    value->~T();
                    slot.stamp.store(head + one_lap_,
                                     std::memory_order_release);
                    return PopStatus::Ok;
                }
            } else if (stamp == head) {
                const std::size_t tail = tail_.load(std::memory_order_seq_cst);
                if ((tail & ~mark_bit_) == head) {
                    return (tail & mark_bit_) != 0 ? PopStatus::Closed
                                                   : PopStatus::Empty;
                }
                backoff(spins);
                head = head_.load(std::memory_order_relaxed);
            } else {
                backoff(spins);
                head = head_.load(std::memory_order_relaxed);
            }
        }
    }

    void mark_closed() noexcept {
        tail_.fetch_or(mark_bit_, std::memory_order_seq_cst);
    }

    std::size_t capacity() const noexcept { return capacity_; }

    std::size_t size() const noexcept {
        for (;;) {
            const std::size_t tail = tail_.load(std::memory_order_seq_cst);
            const std::size_t head = head_.load(std::memory_order_seq_cst);
            if (tail_.load(std::memory_order_seq_cst) != tail) continue;
            const std::size_t head_index = head & (mark_bit_ - 1);
            const std::size_t tail_index = tail & (mark_bit_ - 1);
            if (head_index < tail_index) return tail_index - head_index;
            if (head_index > tail_index) {
                return capacity_ - head_index + tail_index;
            }
            return (tail & ~mark_bit_) == head ? 0 : capacity_;
        }
    }

   private:
    struct Slot {
        std::atomic<std::size_t> stamp{0};
        alignas(T) unsigned char storage[sizeof(T)];
    };

    static std::size_t next_power_of_two(std::size_t value) noexcept {
        std::size_t power = 1;
        while (power < value) power <<= 1;
        return power;
    }

    // The other side won a slot and has not published yet; it is about to.
    static void backoff(int& spins) noexcept {
        if (++spins > 16) std::this_thread::yield();
    }

    alignas(DFTRACER_UTILS_CACHE_LINE_SIZE) std::atomic<std::size_t> head_{0};
    alignas(DFTRACER_UTILS_CACHE_LINE_SIZE) std::atomic<std::size_t> tail_{0};
    alignas(DFTRACER_UTILS_CACHE_LINE_SIZE) std::size_t capacity_;
    std::size_t mark_bit_;
    std::size_t one_lap_;
    dstd::Vector<Slot> slots_;
};

/// Never full, so it needs a byte budget in the caller. The count is written
/// after the enqueue and read before the dequeue, both seq_cst, because the
/// waiter check of the channel needs that order and moodycamel does not give
/// it.
template <typename T>
class UnboundedQueue {
   public:
    using value_type = T;

    explicit UnboundedQueue(std::size_t initial_size) : queue_(initial_size) {}

    PushStatus try_push(T& item) {
        pushing_.fetch_add(1, std::memory_order_seq_cst);
        if (closed_.load(std::memory_order_seq_cst)) {
            pushing_.fetch_sub(1, std::memory_order_seq_cst);
            return PushStatus::Closed;
        }
        queue_.enqueue(std::move(item));
        count_.fetch_add(1, std::memory_order_seq_cst);
        pushing_.fetch_sub(1, std::memory_order_seq_cst);
        return PushStatus::Ok;
    }

    PopStatus try_pop(dstd::Optional<T>& out) {
        for (;;) {
            const bool closed = closed_.load(std::memory_order_seq_cst);
            const bool idle = pushing_.load(std::memory_order_seq_cst) == 0;
            if (count_.load(std::memory_order_seq_cst) > 0) {
                T value;
                if (queue_.try_dequeue(value)) {
                    count_.fetch_sub(1, std::memory_order_seq_cst);
                    out.emplace(std::move(value));
                    return PopStatus::Ok;
                }
                // Another consumer holds the item and has not counted it yet,
                // or moodycamel failed under a consumer race. Empty here could
                // park a receiver next to an item, so try again.
            } else {
                if (!closed) return PopStatus::Empty;
                if (idle) return PopStatus::Closed;
            }
            std::this_thread::yield();
        }
    }

    void mark_closed() noexcept {
        closed_.store(true, std::memory_order_seq_cst);
    }

    std::size_t capacity() const noexcept { return SIZE_MAX; }

    std::size_t size() const noexcept {
        return count_.load(std::memory_order_seq_cst);
    }

   private:
    dstd::ConcurrentQueue<T> queue_;
    alignas(DFTRACER_UTILS_CACHE_LINE_SIZE) std::atomic<std::size_t> count_{0};
    alignas(DFTRACER_UTILS_CACHE_LINE_SIZE) std::atomic<std::size_t> pushing_{
        0};
    std::atomic<bool> closed_{false};
};

}  // namespace dftracer::utils::coro::detail

#endif  // DFTRACER_UTILS_CORO_DETAIL_CHANNEL_QUEUE_H
