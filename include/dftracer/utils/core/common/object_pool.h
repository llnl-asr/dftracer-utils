#ifndef DFTRACER_UTILS_CORE_COMMON_OBJECT_POOL_H
#define DFTRACER_UTILS_CORE_COMMON_OBJECT_POOL_H

#include <dftracer/utils/core/common/platform_compat.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <unordered_map>

namespace dftracer::utils {

/**
 * @brief Lock-free LIFO stack (Treiber stack) with ABA-safe tagged pointers.
 *
 * Intrusive: the `next` pointer is stored in the first 8 bytes of the
 * block itself (valid since blocks are at least sizeof(void*) bytes).
 *
 * ABA protection: x86-64 uses 48-bit virtual addresses. A 16-bit
 * generation counter is packed into the upper bits of a 64-bit atomic.
 *
 * Reference: Treiber, R.K. (1986) "Systems Programming: Coping with
 * Parallelism", IBM Technical Report.
 */
class TreiberStack {
    // Intrusive next-pointer stored in the first sizeof(void*) bytes of
    // freed blocks. Accessed via atomic_ref so TSAN can track the
    // happens-before relationship through the CAS on head_.
    static void store_next(void* block, void* next) noexcept {
        std::atomic_ref<void*>(*reinterpret_cast<void**>(block))
            .store(next, std::memory_order_release);
    }
    static void* load_next(void* block) noexcept {
        return std::atomic_ref<void*>(*reinterpret_cast<void**>(block))
            .load(std::memory_order_acquire);
    }

    std::atomic<std::uint64_t> head_;

    static constexpr std::uint64_t PTR_MASK = 0x0000FFFFFFFFFFFFULL;

   public:
    TreiberStack() noexcept : head_{pack(nullptr, 0)} {}

    void push(void* block) noexcept {
        auto old_head = head_.load(std::memory_order_relaxed);
        std::uint64_t new_head;
        do {
            store_next(block, unpack_ptr(old_head));
            new_head = pack(block, unpack_tag(old_head) + 1);
        } while (!head_.compare_exchange_weak(old_head, new_head,
                                              std::memory_order_release,
                                              std::memory_order_relaxed));
    }

    void* pop() noexcept {
        auto old_head = head_.load(std::memory_order_acquire);
        std::uint64_t new_head;
        void* block;
        do {
            block = unpack_ptr(old_head);
            if (!block) return nullptr;
            void* next = load_next(block);
            new_head = pack(next, unpack_tag(old_head) + 1);
        } while (!head_.compare_exchange_weak(old_head, new_head,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed));
        return block;
    }

   private:
    static constexpr int TAG_SHIFT = 48;

    static std::uint64_t pack(void* ptr, std::uint64_t tag) noexcept {
        auto raw = reinterpret_cast<std::uintptr_t>(ptr) & PTR_MASK;
        return raw | (tag << TAG_SHIFT);
    }

    static void* unpack_ptr(std::uint64_t packed) noexcept {
        auto raw = static_cast<std::uintptr_t>(packed & PTR_MASK);
        // Sign-extend bit 47 for canonical x86-64 addresses
        if (raw & (1ULL << 47)) {
            raw |= ~PTR_MASK;
        }
        return reinterpret_cast<void*>(raw);
    }

    static std::uint64_t unpack_tag(std::uint64_t packed) noexcept {
        return packed >> TAG_SHIFT;
    }
};

/**
 * @brief Thread-safe, lock-free object pool with size-bucketed freelists.
 *
 * Uses TreiberStack (LIFO) per size class. After warmup, allocations are
 * zero-malloc: freed blocks are recycled immediately.
 *
 * Usage:
 * @code
 *   void* p = ObjectPool::instance().allocate(256);
 *   ObjectPool::instance().deallocate(p, 256);
 * @endcode
 */
class ObjectPool {
   public:
    static ObjectPool& instance() {
        static ObjectPool pool;
        return pool;
    }

    void* allocate(std::size_t size) {
        auto& stack = get_stack(size);
        void* block = stack.pop();
        if (block) return block;
        return ::operator new(size);
    }

    void deallocate(void* block, std::size_t size) {
        auto& stack = get_stack(size);
        stack.push(block);
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

   private:
    ObjectPool() = default;

    ~ObjectPool() {
        // Drain all fast buckets
        for (auto& stack : fast_buckets_) {
            while (void* block = stack.pop()) {
                ::operator delete(block);
            }
        }
        // Drain all slow buckets
        for (auto& [_, stack] : slow_buckets_) {
            while (void* block = stack.pop()) {
                ::operator delete(block);
            }
        }
    }

    static constexpr std::size_t ALIGNMENT = 8;
    static constexpr std::size_t MAX_FAST_SIZE = 4096;
    static constexpr std::size_t NUM_FAST_BUCKETS = MAX_FAST_SIZE / ALIGNMENT;

    std::array<TreiberStack, NUM_FAST_BUCKETS> fast_buckets_;

    std::mutex slow_mutex_;
    std::unordered_map<std::size_t, TreiberStack> slow_buckets_;

    TreiberStack& get_stack(std::size_t size) {
        std::size_t bucket = (size + ALIGNMENT - 1) / ALIGNMENT;
        if (bucket > 0 && bucket <= NUM_FAST_BUCKETS) {
            return fast_buckets_[bucket - 1];
        }
        std::lock_guard<std::mutex> lock(slow_mutex_);
        return slow_buckets_[bucket];
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_OBJECT_POOL_H
