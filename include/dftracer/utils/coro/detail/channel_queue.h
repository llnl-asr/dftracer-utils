// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_DETAIL_CHANNEL_QUEUE_H
#define DFTRACER_UTILS_CORO_DETAIL_CHANNEL_QUEUE_H

#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
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

/// Never full, so it needs a byte budget in the caller. Linked blocks from our
/// allocator, with one tail index and one head index, so items arrive in one
/// global order. A receiver that claims a slot whose producer has not finished
/// waits for it, so Empty never parks a receiver next to an item. The tail CAS
/// and its load are seq_cst because the waiter check of the channel needs that
/// order.
///
/// After the list flavor of crossbeam-channel,
/// https://github.com/crossbeam-rs/crossbeam, MIT OR Apache-2.0.
template <typename T>
class UnboundedQueue {
   public:
    using value_type = T;

    UnboundedQueue() {
        Block* first = make_block();
        head_block_.store(first, std::memory_order_relaxed);
        tail_block_.store(first, std::memory_order_relaxed);
    }

    UnboundedQueue(const UnboundedQueue&) = delete;
    UnboundedQueue& operator=(const UnboundedQueue&) = delete;

    ~UnboundedQueue() {
        std::size_t head =
            head_index_.load(std::memory_order_relaxed) & ~MARK_BIT;
        const std::size_t tail =
            tail_index_.load(std::memory_order_relaxed) & ~MARK_BIT;
        Block* block = head_block_.load(std::memory_order_relaxed);
        for (; head != tail; head += ONE_INDEX) {
            const std::size_t offset = (head >> SHIFT) % LAP;
            if (offset < BLOCK_CAP) {
                std::destroy_at(block->slots[offset].item());
            } else {
                Block* next = block->next.load(std::memory_order_relaxed);
                free_block(block);
                block = next;
            }
        }
        free_block(block);
    }

    PushStatus try_push(T& item) {
        std::size_t tail = tail_index_.load(std::memory_order_acquire);
        Block* block = tail_block_.load(std::memory_order_acquire);
        Block* next = nullptr;
        std::size_t failures = 0;
        for (;;) {
            if ((tail & MARK_BIT) != 0) {
                if (next != nullptr) free_block(next);
                return PushStatus::Closed;
            }
            const std::size_t offset = (tail >> SHIFT) % LAP;
            if (offset == BLOCK_CAP) {
                std::this_thread::yield();
                tail = tail_index_.load(std::memory_order_acquire);
                block = tail_block_.load(std::memory_order_acquire);
                continue;
            }
            // Allocate before the claim, so other producers wait less.
            if (offset + 1 == BLOCK_CAP && next == nullptr) next = make_block();
            if (tail_index_.compare_exchange_weak(tail, tail + ONE_INDEX,
                                                  std::memory_order_seq_cst,
                                                  std::memory_order_acquire)) {
                if (offset + 1 == BLOCK_CAP) {
                    tail_block_.store(next, std::memory_order_release);
                    tail_index_.fetch_add(ONE_INDEX, std::memory_order_release);
                    block->next.store(next, std::memory_order_release);
                    next = nullptr;
                }
                Slot& slot = block->slots[offset];
                std::construct_at(slot.item(), std::move(item));
                slot.state.fetch_or(WRITE, std::memory_order_release);
                if (next != nullptr) free_block(next);
                return PushStatus::Ok;
            }
            back_off(failures);
            block = tail_block_.load(std::memory_order_acquire);
        }
    }

    PopStatus try_pop(dstd::Optional<T>& out) {
        std::size_t head = head_index_.load(std::memory_order_acquire);
        Block* block = head_block_.load(std::memory_order_acquire);
        std::size_t failures = 0;
        for (;;) {
            const std::size_t offset = (head >> SHIFT) % LAP;
            if (offset == BLOCK_CAP) {
                std::this_thread::yield();
                head = head_index_.load(std::memory_order_acquire);
                block = head_block_.load(std::memory_order_acquire);
                continue;
            }
            std::size_t new_head = head + ONE_INDEX;
            if ((new_head & MARK_BIT) == 0) {
                const std::size_t tail =
                    tail_index_.load(std::memory_order_seq_cst);
                if ((head >> SHIFT) == (tail >> SHIFT)) {
                    return (tail & MARK_BIT) != 0 ? PopStatus::Closed
                                                  : PopStatus::Empty;
                }
                if ((head >> SHIFT) / LAP != (tail >> SHIFT) / LAP) {
                    new_head |= MARK_BIT;
                }
            }
            if (head_index_.compare_exchange_weak(head, new_head,
                                                  std::memory_order_seq_cst,
                                                  std::memory_order_acquire)) {
                if (offset + 1 == BLOCK_CAP) advance_head(block, new_head);
                Slot& slot = block->slots[offset];
                while ((slot.state.load(std::memory_order_acquire) & WRITE) ==
                       0) {
                    std::this_thread::yield();
                }
                out.emplace(std::move(*slot.item()));
                std::destroy_at(slot.item());
                if (offset + 1 == BLOCK_CAP) {
                    release_block(block, 0);
                } else if ((slot.state.fetch_or(READ,
                                                std::memory_order_acq_rel) &
                            DESTROY) != 0) {
                    release_block(block, offset + 1);
                }
                return PopStatus::Ok;
            }
            back_off(failures);
            block = head_block_.load(std::memory_order_acquire);
        }
    }

    void mark_closed() noexcept {
        tail_index_.fetch_or(MARK_BIT, std::memory_order_seq_cst);
    }

    std::size_t capacity() const noexcept { return SIZE_MAX; }

    std::size_t size() const noexcept {
        for (;;) {
            std::size_t tail = tail_index_.load(std::memory_order_seq_cst);
            std::size_t head = head_index_.load(std::memory_order_seq_cst);
            if (tail_index_.load(std::memory_order_seq_cst) != tail) continue;
            tail &= ~MARK_BIT;
            head &= ~MARK_BIT;
            if ((tail >> SHIFT) % LAP == BLOCK_CAP) tail += ONE_INDEX;
            if ((head >> SHIFT) % LAP == BLOCK_CAP) head += ONE_INDEX;
            const std::size_t skipped = ((head >> SHIFT) / LAP) * LAP;
            tail = (tail >> SHIFT) - skipped;
            head = (head >> SHIFT) - skipped;
            return tail - head - tail / LAP;
        }
    }

   private:
    static constexpr std::size_t SPIN_LIMIT = 6;
    static constexpr std::size_t WRITE = 1;
    static constexpr std::size_t READ = 2;
    static constexpr std::size_t DESTROY = 4;
    /// One index of each lap has no slot: it marks the wait for the next block.
    static constexpr std::size_t LAP = 32;
    static constexpr std::size_t BLOCK_CAP = LAP - 1;
    static constexpr std::size_t SHIFT = 1;
    static constexpr std::size_t ONE_INDEX = std::size_t{1} << SHIFT;
    /// In the tail index: closed. In the head index: not the last block, so a
    /// receiver need not read the tail.
    static constexpr std::size_t MARK_BIT = 1;

    struct Slot {
        std::atomic<std::size_t> state{0};
        alignas(T) unsigned char storage[sizeof(T)];

        T* item() noexcept { return reinterpret_cast<T*>(storage); }
    };

    struct Block {
        std::atomic<Block*> next{nullptr};
        Slot slots[BLOCK_CAP];
    };

    /// Exponential, as crossbeam's Backoff::spin. A failed CAS means a
    /// contended line, and an immediate retry adds to it.
    static void back_off(std::size_t& failures) noexcept {
        const std::size_t spins = std::size_t{1}
                                  << std::min(failures, SPIN_LIMIT);
        for (std::size_t i = 0; i < spins; ++i) DFTRACER_UTILS_CPU_PAUSE();
        ++failures;
    }

    static Block* make_block() { return dstd::make_unique<Block>().release(); }

    static void free_block(Block* block) noexcept {
        dstd::Delete<Block>{}(block);
    }

    void advance_head(Block* block, std::size_t new_head) noexcept {
        Block* next = block->next.load(std::memory_order_acquire);
        while (next == nullptr) {
            std::this_thread::yield();
            next = block->next.load(std::memory_order_acquire);
        }
        std::size_t index = (new_head & ~MARK_BIT) + ONE_INDEX;
        if (next->next.load(std::memory_order_relaxed) != nullptr) {
            index |= MARK_BIT;
        }
        head_block_.store(next, std::memory_order_release);
        head_index_.store(index, std::memory_order_release);
    }

    /// The last receiver of a block frees it. A slot still being read carries
    /// DESTROY, and its reader continues from there.
    static void release_block(Block* block, std::size_t start) noexcept {
        for (std::size_t i = start; i + 1 < BLOCK_CAP; ++i) {
            Slot& slot = block->slots[i];
            if ((slot.state.load(std::memory_order_acquire) & READ) == 0 &&
                (slot.state.fetch_or(DESTROY, std::memory_order_acq_rel) &
                 READ) == 0) {
                return;
            }
        }
        free_block(block);
    }

    alignas(
        DFTRACER_UTILS_CACHE_LINE_SIZE) std::atomic<std::size_t> head_index_{0};
    std::atomic<Block*> head_block_{nullptr};
    alignas(
        DFTRACER_UTILS_CACHE_LINE_SIZE) std::atomic<std::size_t> tail_index_{0};
    std::atomic<Block*> tail_block_{nullptr};
};

}  // namespace dftracer::utils::coro::detail

#endif  // DFTRACER_UTILS_CORO_DETAIL_CHANNEL_QUEUE_H
