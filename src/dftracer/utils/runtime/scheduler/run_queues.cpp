// SPDX-License-Identifier: MIT
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/runtime/scheduler/run_queues.h>

#include <algorithm>

namespace dftracer::utils::runtime::scheduler {

void InjectQueue::grow_locked(std::size_t needed) {
    const std::size_t size = size_.load(std::memory_order_relaxed);
    if (size + needed <= ring_.size()) return;
    // The ring outlives the pool scope of whoever made it grow.
    memory::detail::Scope bookkeeping{memory::detail::Manager::get().general(),
                                      nullptr};
    dstd::Vector<void*> bigger(std::max<std::size_t>(
        {std::size_t{512}, ring_.size() * 2, size + needed}));
    for (std::size_t i = 0; i < size; ++i) {
        bigger[i] = ring_[(head_ + i) % ring_.size()];
    }
    ring_.swap(bigger);
    head_ = 0;
}

void InjectQueue::push(void* task) {
    std::lock_guard<std::mutex> lock(mutex_);
    grow_locked(1);
    const std::size_t size = size_.load(std::memory_order_relaxed);
    ring_[(head_ + size) % ring_.size()] = task;
    size_.store(size + 1, std::memory_order_release);
}

void InjectQueue::push_batch(void* const* tasks, std::size_t count,
                             void* last) {
    std::lock_guard<std::mutex> lock(mutex_);
    grow_locked(count + 1);
    std::size_t size = size_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < count; ++i) {
        ring_[(head_ + size++) % ring_.size()] = tasks[i];
    }
    ring_[(head_ + size++) % ring_.size()] = last;
    size_.store(size, std::memory_order_release);
}

std::size_t InjectQueue::pop_batch(void** out, std::size_t limit) {
    if (size_.load(std::memory_order_acquire) == 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t size = size_.load(std::memory_order_relaxed);
    const std::size_t count = std::min(size, limit);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = ring_[head_];
        head_ = (head_ + 1) % ring_.size();
    }
    size_.store(size - count, std::memory_order_release);
    return count;
}

void LocalQueue::push_back(void* task, InjectQueue& overflow) {
    const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
    std::uint64_t head = head_.load(std::memory_order_acquire);
    for (;;) {
        const std::uint32_t steal = steal_of(head);
        const std::uint32_t real = real_of(head);
        if (tail - steal < CAPACITY) {
            slots_[tail & MASK] = task;
            tail_.store(tail + 1, std::memory_order_release);
            return;
        }
        if (steal != real) {
            // A thief holds half the ring and frees it soon.
            overflow.push(task);
            return;
        }
        const std::uint32_t moved = CAPACITY / 2;
        const std::uint64_t next = pack(real + moved, real + moved);
        if (head_.compare_exchange_weak(head, next, std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            void* spilled[CAPACITY / 2];
            for (std::uint32_t i = 0; i < moved; ++i) {
                spilled[i] = slots_[(real + i) & MASK];
            }
            overflow.push_batch(spilled, moved, task);
            return;
        }
    }
}

void* LocalQueue::pop() noexcept {
    const std::uint32_t tail = tail_.load(std::memory_order_relaxed);
    std::uint64_t head = head_.load(std::memory_order_acquire);
    for (;;) {
        const std::uint32_t steal = steal_of(head);
        const std::uint32_t real = real_of(head);
        if (real == tail) return nullptr;
        const std::uint32_t next_real = real + 1;
        const std::uint64_t next =
            steal == real ? pack(next_real, next_real) : pack(steal, next_real);
        if (head_.compare_exchange_weak(head, next, std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            return slots_[real & MASK];
        }
    }
}

void* LocalQueue::steal_into(LocalQueue& into) noexcept {
    const std::uint32_t into_tail = into.tail_.load(std::memory_order_relaxed);
    const std::uint32_t into_steal =
        steal_of(into.head_.load(std::memory_order_acquire));
    // Only a nearly empty thief steals, so the copy below always fits.
    if (into_tail - into_steal > CAPACITY / 2) return nullptr;

    std::uint64_t head = head_.load(std::memory_order_acquire);
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    for (;;) {
        const std::uint32_t steal = steal_of(head);
        const std::uint32_t real = real_of(head);
        if (steal != real) return nullptr;
        const std::uint32_t tail = tail_.load(std::memory_order_acquire);
        const std::uint32_t available = tail - real;
        count = available - available / 2;
        if (count == 0) return nullptr;
        const std::uint64_t claimed = pack(steal, real + count);
        if (head_.compare_exchange_weak(head, claimed,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            first = real;
            break;
        }
    }

    for (std::uint32_t i = 0; i < count; ++i) {
        into.slots_[(into_tail + i) & MASK] = slots_[(first + i) & MASK];
    }

    // The owner may have popped past the claim while the copy ran.
    head = head_.load(std::memory_order_acquire);
    for (;;) {
        const std::uint32_t real = real_of(head);
        if (head_.compare_exchange_weak(head, pack(real, real),
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            break;
        }
    }

    void* task = into.slots_[(into_tail + count - 1) & MASK];
    if (count > 1) {
        into.tail_.store(into_tail + count - 1, std::memory_order_release);
    }
    return task;
}

std::uint32_t LocalQueue::size_approx() const noexcept {
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    const std::uint32_t tail = tail_.load(std::memory_order_acquire);
    return tail - real_of(head);
}

}  // namespace dftracer::utils::runtime::scheduler
