#ifndef DFTRACER_UTILS_RUNTIME_EXECUTOR_PARKED_H
#define DFTRACER_UTILS_RUNTIME_EXECUTOR_PARKED_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>

namespace dftracer::utils {

/// Wakes one parked waiter so its await_resume reports cancellation. Called
/// with one registry shard lock held, possibly more than once and concurrently
/// with the awaitable's own wake path: it must wake the waiter exactly once
/// overall and do nothing when it was already woken. Because a waiter
/// registers while it holds its awaitable's lock, it must only try-lock that
/// lock and return when it is busy; shutdown calls again. Must not call park()
/// or unpark().
using ParkCancelFn = void (*)(void* waiter) noexcept;

/// The registry entry of a coroutine suspended on an awaitable that only
/// another party can finish. The awaitable registers once the waiter is
/// queued, so executor shutdown can wake the waiter with a cancelled status
/// instead of leaving the frame suspended for ever. The entry is allocated at
/// that point, so a path that never queues pays only for a null pointer.
struct ParkNode {
    ParkNode(ParkCancelFn c, void* w) noexcept
        : cancel(c),
          waiter(w),
          cancelled(false),
          prev(nullptr),
          next(nullptr) {}

    ParkCancelFn cancel;
    void* waiter;
    /// Free for the awaitable to use; set by its cancel callback.
    bool cancelled;

   private:
    friend class ParkRegistry;
    ParkNode* prev;
    ParkNode* next;
};

/// The waiters of one executor. Thread-safe. Sharded by node address so that
/// waiters on different threads do not share a lock.
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
