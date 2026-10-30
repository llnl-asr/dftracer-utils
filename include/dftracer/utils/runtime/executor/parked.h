#ifndef DFTRACER_UTILS_RUNTIME_EXECUTOR_PARKED_H
#define DFTRACER_UTILS_RUNTIME_EXECUTOR_PARKED_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>

namespace dftracer::utils {

/// Wakes one parked waiter with a cancelled status. Called with a registry
/// shard lock held, possibly concurrently with the awaitable's own wake path,
/// so it must wake the waiter exactly once. It must only try-lock the
/// awaitable's lock and return if busy (shutdown calls again), and not call
/// park()/unpark().
using ParkCancelFn = void (*)(void* waiter) noexcept;

/// The registry entry of a coroutine suspended on an awaitable that only
/// another party can finish. Shutdown wakes the waiter with a cancelled status
/// through it.
struct ParkNode {
    ParkNode(ParkCancelFn c, void* w) noexcept
        : cancel(c),
          waiter(w),
          cancelled(false),
          prev(nullptr),
          next(nullptr) {}

    ParkCancelFn cancel;
    void* waiter;
    /// Free for the awaitable to use. Set by its cancel callback.
    bool cancelled;

   private:
    friend class ParkRegistry;
    ParkNode* prev;
    ParkNode* next;
};

/// The waiters of one executor. Thread-safe.
class ParkRegistry {
   public:
    ParkRegistry() = default;
    ParkRegistry(const ParkRegistry&) = delete;
    ParkRegistry& operator=(const ParkRegistry&) = delete;

    /// Registers `waiter`. Returns null, and the waiter stays unregistered,
    /// when there is no memory for the entry.
    ParkNode* park(void* waiter, ParkCancelFn cancel) noexcept {
        auto* n = new (std::nothrow) ParkNode(cancel, waiter);
        if (n == nullptr) return nullptr;
        Shard& s = shard_of(*n);
        std::lock_guard<std::mutex> lock(s.mutex);
        n->next = s.head;
        if (s.head) s.head->prev = n;
        s.head = n;
        s.size.store(s.size.load(std::memory_order_relaxed) + 1,
                     std::memory_order_release);
        return n;
    }

    /// Removes and frees `n`. Blocks while cancel_all() runs over the same
    /// shard. Null is ignored.
    void unpark(ParkNode* n) noexcept {
        if (n == nullptr) return;
        Shard& s = shard_of(*n);
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (n->prev) {
                n->prev->next = n->next;
            } else {
                s.head = n->next;
            }
            if (n->next) n->next->prev = n->prev;
            s.size.store(s.size.load(std::memory_order_relaxed) - 1,
                         std::memory_order_release);
        }
        delete n;
    }

    /// Wakes every registered waiter. A waiter leaves the registry when its
    /// frame resumes.
    void cancel_all() noexcept {
        for (Shard& s : shards_) {
            if (s.size.load(std::memory_order_acquire) == 0) continue;
            std::lock_guard<std::mutex> lock(s.mutex);
            for (ParkNode* n = s.head; n; n = n->next) {
                n->cancel(n->waiter);
            }
        }
    }

    /// Waiters registered now. Takes no lock.
    std::size_t size() const noexcept {
        std::size_t n = 0;
        for (const Shard& s : shards_) {
            n += s.size.load(std::memory_order_acquire);
        }
        return n;
    }

   private:
    static constexpr std::size_t SHARDS = 16;

    struct alignas(64) Shard {
        std::mutex mutex;
        ParkNode* head = nullptr;
        std::atomic<std::size_t> size{0};
    };

    Shard& shard_of(const ParkNode& n) noexcept {
        const auto a = reinterpret_cast<std::uintptr_t>(&n);
        return shards_[(a >> 6) % SHARDS];
    }

    Shard shards_[SHARDS];
};

}  // namespace dftracer::utils

#endif  // DFTRACER_UTILS_RUNTIME_EXECUTOR_PARKED_H
