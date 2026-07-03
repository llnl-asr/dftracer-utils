#ifndef DFTRACER_UTILS_CORE_COMMON_OBJECT_POOL_H
#define DFTRACER_UTILS_CORE_COMMON_OBJECT_POOL_H

#include <dftracer/utils/core/common/platform_compat.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace dftracer::utils {

namespace detail {

struct TreiberBase {
    static void store_next(void* block, void* next) noexcept {
        __atomic_store_n(reinterpret_cast<void**>(block), next,
                         __ATOMIC_RELEASE);
    }
    static void* load_next(void* block) noexcept {
        return __atomic_load_n(reinterpret_cast<void**>(block),
                               __ATOMIC_ACQUIRE);
    }
};

struct alignas(16) TaggedHead {
    void* ptr;
    std::uint64_t tag;
};

// ABA-safe via a 16-byte compare-and-swap (CMPXCHG16B on x86-64 with -mcx16,
// CASP/LSE on AArch64 with -march=armv8.1-a+). The pointer is stored verbatim,
// so it works on any address layout. Used only when the 16-byte atomic is
// lock-free; otherwise TreiberStackPacked is selected.
class TreiberStackDwcas : TreiberBase {
    std::atomic<TaggedHead> head_;

   public:
    TreiberStackDwcas() noexcept : head_{TaggedHead{nullptr, 0}} {}

    void push(void* block) noexcept {
        auto old_head = head_.load(std::memory_order_relaxed);
        TaggedHead new_head;
        do {
            store_next(block, old_head.ptr);
            new_head = TaggedHead{block, old_head.tag + 1};
        } while (!head_.compare_exchange_weak(old_head, new_head,
                                              std::memory_order_release,
                                              std::memory_order_relaxed));
    }

    void* pop() noexcept {
        auto old_head = head_.load(std::memory_order_acquire);
        TaggedHead new_head;
        do {
            if (!old_head.ptr) return nullptr;
            void* next = load_next(old_head.ptr);
            new_head = TaggedHead{next, old_head.tag + 1};
        } while (!head_.compare_exchange_weak(old_head, new_head,
                                              std::memory_order_acquire,
                                              std::memory_order_acquire));
        return old_head.ptr;
    }
};

// Fallback without a lock-free 16-byte CAS: pack the pointer plus a 16-bit ABA
// tag into one 64-bit atomic. The pointer is recovered by masking only (zero
// extend); it must NOT be sign-extended from bit 47, since AArch64 user
// pointers legitimately have bit 47 set (the kernel/user split is bit 55, not
// 47) and sign-extending corrupts them. Assumes a 48-bit VA; no 52-bit/LVA.
class TreiberStackPacked : TreiberBase {
    std::atomic<std::uint64_t> head_;

    static constexpr std::uint64_t PTR_MASK = 0x0000FFFFFFFFFFFFULL;
    static constexpr int TAG_SHIFT = 48;

    static std::uint64_t pack(void* ptr, std::uint64_t tag) noexcept {
        auto raw = reinterpret_cast<std::uintptr_t>(ptr) & PTR_MASK;
        return raw | (tag << TAG_SHIFT);
    }
    static void* unpack_ptr(std::uint64_t packed) noexcept {
        return reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(packed & PTR_MASK));
    }
    static std::uint64_t unpack_tag(std::uint64_t packed) noexcept {
        return packed >> TAG_SHIFT;
    }

   public:
    TreiberStackPacked() noexcept : head_{pack(nullptr, 0)} {}

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
                                              std::memory_order_acquire));
        return block;
    }
};

}  // namespace detail

// DWCAS when the 16-byte atomic is lock-free, else the packed fallback.
using TreiberStack =
    std::conditional_t<std::atomic<detail::TaggedHead>::is_always_lock_free,
                       detail::TreiberStackDwcas, detail::TreiberStackPacked>;

class ObjectPool {
   public:
    static ObjectPool& instance() {
        static ObjectPool pool;
        return pool;
    }

    void* allocate(std::size_t size) {
        auto* stack = get_stack(size);
        if (!stack) return ::operator new(size);
        // Hand out the payload past the reserved header, so
        // load_next/store_next (the only accessors of `next`) never race the
        // caller's writes.
        if (void* block = stack->pop()) {
            return static_cast<char*>(block) + BLOCK_HEADER;
        }
        void* block = ::operator new(BLOCK_HEADER + size);
        return static_cast<char*>(block) + BLOCK_HEADER;
    }

    void deallocate(void* ptr, std::size_t size) {
        auto* stack = get_stack(size);
        if (!stack) {
            ::operator delete(ptr);
            return;
        }
        stack->push(static_cast<char*>(ptr) - BLOCK_HEADER);
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

   private:
    ObjectPool() = default;

    ~ObjectPool() {
        for (auto& stack : fast_buckets_) {
            while (void* block = stack.pop()) {
                ::operator delete(block);
            }
        }
        for (auto& slot : slow_table_) {
            if (slot.bucket.load(std::memory_order_relaxed) != 0) {
                while (void* block = slot.stack.pop()) {
                    ::operator delete(block);
                }
            }
        }
    }

    // Reserved header holding the free-list `next`; max-aligned so the payload
    // past it keeps default-new alignment.
    static constexpr std::size_t BLOCK_HEADER = alignof(std::max_align_t);
    static_assert(BLOCK_HEADER >= sizeof(void*),
                  "block header must hold a next pointer");

    static constexpr std::size_t ALIGNMENT = 8;
    static constexpr std::size_t MAX_FAST_SIZE = 4096;
    static constexpr std::size_t NUM_FAST_BUCKETS = MAX_FAST_SIZE / ALIGNMENT;

    static constexpr std::size_t SLOW_TABLE_SIZE = 256;
    static constexpr std::size_t SLOW_TABLE_MASK = SLOW_TABLE_SIZE - 1;

    std::array<TreiberStack, NUM_FAST_BUCKETS> fast_buckets_;

    struct SlowSlot {
        std::atomic<std::size_t> bucket{0};
        TreiberStack stack;
    };
    std::array<SlowSlot, SLOW_TABLE_SIZE> slow_table_;

    TreiberStack* get_stack(std::size_t size) {
        std::size_t bucket = (size + ALIGNMENT - 1) / ALIGNMENT;
        if (bucket > 0 && bucket <= NUM_FAST_BUCKETS) {
            return &fast_buckets_[bucket - 1];
        }
        return find_slow_stack(bucket);
    }

    TreiberStack* find_slow_stack(std::size_t bucket) {
        auto h = bucket;
        for (std::size_t i = 0; i < SLOW_TABLE_SIZE; ++i) {
            auto idx = (h + i) & SLOW_TABLE_MASK;
            auto& slot = slow_table_[idx];
            auto existing = slot.bucket.load(std::memory_order_acquire);
            if (existing == bucket) return &slot.stack;
            if (existing == 0) {
                std::size_t expected = 0;
                if (slot.bucket.compare_exchange_strong(
                        expected, bucket, std::memory_order_release,
                        std::memory_order_acquire)) {
                    return &slot.stack;
                }
                if (slot.bucket.load(std::memory_order_acquire) == bucket) {
                    return &slot.stack;
                }
            }
        }
        return nullptr;
    }
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_CORE_COMMON_OBJECT_POOL_H
