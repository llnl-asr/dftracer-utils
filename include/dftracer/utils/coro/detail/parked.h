// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_DETAIL_PARKED_H
#define DFTRACER_UTILS_CORO_DETAIL_PARKED_H

#include <dftracer/utils/dstd/memory.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>

namespace dftracer::utils::coro::detail {

/// Wakes one parked waiter with a cancelled status. Called with a registry
/// shard lock held, possibly concurrently with the awaitable's own wake path,
/// so it must wake the waiter exactly once. It must only try-lock the
/// awaitable's lock and return if busy (shutdown calls again), and not call
/// park()/unpark().
using ParkCancelFn = void (*)(void* waiter) noexcept;

/// The task that is suspending now, so a registry can tell later that it was
/// cancelled. Valid while its node is registered.
struct ParkOwner {
    const void* promise{nullptr};
    bool (*cancel_requested)(const void*) noexcept {nullptr};
};

template <typename Promise>
bool promise_cancel_requested(const void* promise) noexcept {
    return static_cast<const Promise*>(promise)->cancel_requested();
}

/// The owner of the suspend running on this thread. In a source file, so every
/// library sees one thread local.
ParkOwner current_park_owner() noexcept;

/// Notes that a waiter parked after its task was cancelled.
void note_park_after_cancel() noexcept;

/// Names the owner of a suspend. An empty owner keeps a waiter out of the
/// cancel sweep, for a wait that ignores cancels. A cancel can come after the
/// wait checked for it and before the park, and no sweep then sees the waiter.
/// So the end of this scope sweeps again. It reads only thread locals, since
/// the frame can run by then.
class ParkOwnerScope {
   public:
    explicit ParkOwnerScope(ParkOwner owner) noexcept;
    ~ParkOwnerScope();
    ParkOwnerScope(const ParkOwnerScope&) = delete;
    ParkOwnerScope& operator=(const ParkOwnerScope&) = delete;

   private:
    ParkOwner saved_;
};

template <typename Handle>
ParkOwner park_owner_of(Handle h) noexcept {
    if constexpr (requires { h.promise().cancel_requested(); }) {
        using Promise = std::remove_reference_t<decltype(h.promise())>;
        return {&h.promise(), &promise_cancel_requested<Promise>};
    } else {
        return {};
    }
}

/// The registry entry of a coroutine suspended on an awaitable that only
/// another party can finish. Shutdown wakes the waiter with a cancelled status
/// through it.
struct ParkNode : dstd::HeapObject {
    ParkNode(ParkCancelFn c, void* w) noexcept
        : cancel(c),
          waiter(w),
          cancelled(false),
          prev_(nullptr),
          next_(nullptr) {}

    ParkCancelFn cancel;
    void* waiter;
    ParkOwner owner;
    /// Free for the awaitable to use. Set by its cancel callback.
    bool cancelled;

   private:
    friend class ParkRegistry;
    ParkNode* prev_;
    ParkNode* next_;
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
        n->next_ = s.head;
        if (s.head) s.head->prev_ = n;
        s.head = n;
        s.size.store(s.size.load(std::memory_order_relaxed) + 1,
                     std::memory_order_release);
        n->owner = current_park_owner();
        if (n->owner.promise && n->owner.cancel_requested(n->owner.promise)) {
            note_park_after_cancel();
        }
        return n;
    }

    /// Removes and frees `n`. Blocks while cancel_all() runs over the same
    /// shard. Null is ignored.
    void unpark(ParkNode* n) noexcept {
        if (n == nullptr) return;
        Shard& s = shard_of(*n);
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (n->prev_) {
                n->prev_->next_ = n->next_;
            } else {
                s.head = n->next_;
            }
            if (n->next_) n->next_->prev_ = n->prev_;
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
            for (ParkNode* n = s.head; n; n = n->next_) {
                n->cancel(n->waiter);
            }
        }
    }

    /// Wakes the waiters of cancelled tasks. The cancel callback only
    /// try-locks, so it can miss one, and this goes over them again until they
    /// leave. A callback that ignores cancels keeps its waiter, so the passes
    /// are bounded.
    void cancel_requested_waiters() noexcept {
        for (int pass = 0; pass < 64; ++pass) {
            if (!cancel_requested_pass()) return;
            std::this_thread::yield();
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

    bool cancel_requested_pass() noexcept {
        bool found = false;
        for (Shard& s : shards_) {
            if (s.size.load(std::memory_order_acquire) == 0) continue;
            std::lock_guard<std::mutex> lock(s.mutex);
            for (ParkNode* n = s.head; n; n = n->next_) {
                if (n->owner.promise == nullptr ||
                    !n->owner.cancel_requested(n->owner.promise)) {
                    continue;
                }
                n->cancel(n->waiter);
                found = true;
            }
        }
        return found;
    }

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

}  // namespace dftracer::utils::coro::detail

#endif  // DFTRACER_UTILS_CORO_DETAIL_PARKED_H
