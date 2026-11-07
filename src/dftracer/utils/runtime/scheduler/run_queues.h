// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_RUNTIME_SCHEDULER_RUN_QUEUES_H
#define DFTRACER_UTILS_RUNTIME_SCHEDULER_RUN_QUEUES_H

#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/dstd/vector.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace dftracer::utils::runtime::scheduler {

/// Runnable tasks no worker owns: wakes from other threads and the overflow of
/// a full @ref LocalQueue. FIFO. Thread safe.
class InjectQueue {
   public:
    void push(void* task);
    void push_batch(void* const* tasks, std::size_t count, void* last);

    /// Moves up to @p limit tasks to @p out and returns how many.
    std::size_t pop_batch(void** out, std::size_t limit);

    /// May be stale. Read it with the caller's own ordering.
    std::size_t size_approx() const noexcept {
        return size_.load(std::memory_order_acquire);
    }

   private:
    void grow_locked(std::size_t needed);

    std::mutex mutex_;
    dstd::Vector<void*> ring_;
    std::size_t head_ = 0;
    alignas(DFTRACER_UTILS_OPTIMAL_ALIGNMENT) std::atomic<std::size_t> size_{0};
};

/**
 * @brief The run queue of one worker: its owner pushes and pops, any thread
 * steals half.
 *
 * A fixed ring. A thief first claims a range by moving only the real half of
 * the packed head, copies it, then commits, so at most one steal runs at a time
 * and the owner keeps popping meanwhile. The owner sends half of a full ring to
 * the @ref InjectQueue in one step.
 *
 * Algorithm from the multi-thread scheduler queue of Tokio,
 * https://github.com/tokio-rs/tokio (MIT), file
 * tokio/src/runtime/scheduler/multi_thread/queue.rs.
 */
class LocalQueue {
   public:
    static constexpr std::uint32_t CAPACITY = 256;

    /// Owner only. Never fails: a full ring spills into @p overflow.
    void push_back(void* task, InjectQueue& overflow);

    /// Owner only. Null when empty.
    void* pop() noexcept;

    /// Called by the owner of @p into, on any other queue. Moves about half of
    /// this queue to @p into and returns one of the moved tasks, or null when
    /// there was nothing to take or another steal is running.
    void* steal_into(LocalQueue& into) noexcept;

    /// Tasks this queue holds. May be stale from another thread.
    std::uint32_t size_approx() const noexcept;

   private:
    static constexpr std::uint32_t MASK = CAPACITY - 1;

    static std::uint64_t pack(std::uint32_t steal,
                              std::uint32_t real) noexcept {
        return (static_cast<std::uint64_t>(steal) << 32) | real;
    }
    static std::uint32_t steal_of(std::uint64_t head) noexcept {
        return static_cast<std::uint32_t>(head >> 32);
    }
    static std::uint32_t real_of(std::uint64_t head) noexcept {
        return static_cast<std::uint32_t>(head);
    }

    alignas(DFTRACER_UTILS_OPTIMAL_ALIGNMENT) std::atomic<std::uint64_t> head_{
        0};
    alignas(DFTRACER_UTILS_OPTIMAL_ALIGNMENT) std::atomic<std::uint32_t> tail_{
        0};
    alignas(DFTRACER_UTILS_OPTIMAL_ALIGNMENT) void* slots_[CAPACITY];
};

}  // namespace dftracer::utils::runtime::scheduler

#endif  // DFTRACER_UTILS_RUNTIME_SCHEDULER_RUN_QUEUES_H
