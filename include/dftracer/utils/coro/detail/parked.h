// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_DETAIL_PARKED_H
#define DFTRACER_UTILS_CORO_DETAIL_PARKED_H

#include <dftracer/utils/dstd/memory.h>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>

namespace dftracer::utils::coro {
class Executor;
}  // namespace dftracer::utils::coro

namespace dftracer::utils::coro::detail {

/// Wakes one parked waiter with a cancelled status. Called with a registry
/// shard lock held, possibly concurrently with the awaitable's own wake path,
/// so it must wake the waiter exactly once. It must only try-lock the
/// awaitable's lock and return if busy (shutdown calls again), and not call
/// park()/unpark().
using ParkCancelFn = void (*)(void* waiter) noexcept;

/// The task that is suspending now, so a registry can tell later that it was
/// cancelled. Valid while its node is registered.
struct ParkOwnerOps {
    bool (*cancel_requested)(const void*) noexcept;
    /// Ends the suspended task as cancelled, so the frame never resumes, and
    /// returns true; returns false for a task that must resume, such as one
    /// that holds a scope. Null for an owner that cannot be stopped.
    bool (*stop)(const void*) noexcept;
    /// True for a task that must resume at a cancelled wait, such as one that
    /// holds a scope.
    bool (*refuses_stop)(const void*) noexcept;
};

struct ParkOwner {
    const void* promise{nullptr};
    const ParkOwnerOps* ops{nullptr};
};

static_assert(sizeof(ParkOwner) == 2 * sizeof(void*),
              "ParkOwner is copied on every suspend");

template <typename Promise>
bool promise_cancel_requested(const void* promise) noexcept {
    return static_cast<const Promise*>(promise)->cancel_requested();
}

template <typename Promise>
bool promise_refuses_stop(const void* promise) noexcept {
    return static_cast<const Promise*>(promise)->refuses_stop();
}

template <typename Promise>
bool promise_stop(const void* promise) noexcept {
    auto* p = const_cast<Promise*>(static_cast<const Promise*>(promise));
    if (p->refuses_stop()) return false;
    p->finish_cancelled();
    return true;
}

template <typename Promise>
inline constexpr ParkOwnerOps STOPPABLE_OWNER_OPS{
    &promise_cancel_requested<Promise>, &promise_stop<Promise>,
    &promise_refuses_stop<Promise>};

template <typename Promise>
inline constexpr ParkOwnerOps CANCEL_ONLY_OWNER_OPS{
    &promise_cancel_requested<Promise>, nullptr, nullptr};

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
    using Promise = std::remove_reference_t<decltype(h.promise())>;
    if constexpr (requires {
                      h.promise().finish_cancelled();
                      h.promise().refuses_stop();
                  }) {
        return {&h.promise(), &STOPPABLE_OWNER_OPS<Promise>};
    } else if constexpr (requires { h.promise().cancel_requested(); }) {
        return {&h.promise(), &CANCEL_ONLY_OWNER_OPS<Promise>};
    } else {
        return {};
    }
}

struct ParkNode;

/// Called from a cancel callback, with the waiter already out of its
/// awaitable's queue: posts a job to `executor` that removes `node` and ends
/// the owner task as cancelled, so the waiter never resumes. Returns false,
/// and posts nothing, when the owner cannot be stopped; the caller then
/// resumes the waiter.
bool stop_parked(Executor* executor, ParkNode* node) noexcept;

/// Ends the task `owner` names as cancelled, through a job on `executor` (in
/// place when it is null), and removes `node` first when it is not null.
/// Returns false, and does nothing, when the owner cannot stop. For a finished
/// wait that completes with a stop, where no cancel callback runs. Touches
/// nothing of the waiter after it returns true, since its frame can be gone.
bool stop_task(Executor* executor, ParkNode* node, ParkOwner owner) noexcept;

/// Continues the waiter `handle` of a wait that ended with a stop: ends its
/// task through @ref stop_task, or resumes it when the task cannot stop, and
/// the wait then ends the process.
void stop_or_continue(Executor* executor, ParkOwner owner,
                      std::coroutine_handle<> handle) noexcept;

/// For a cancel callback, after the waiter left its awaitable's queue: ends
/// the owner task through @ref stop_parked and clears `park`, which the stop
/// releases. When the owner cannot stop, keeps `park` and resumes `handle`, and
/// the wait then sees the cancel. Touches nothing of the waiter after a stop is
/// posted, since its frame can be gone by then.
void stop_or_resume(Executor* executor, ParkNode*& park,
                    std::coroutine_handle<> handle) noexcept;

/// Ends the process: a wait was cancelled in a frame that cannot stop, such
/// as one that is not a task. `what` names the wait.
[[noreturn]] void cancelled_unstoppable(const char* what) noexcept;

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
        if (n->owner.promise &&
            n->owner.ops->cancel_requested(n->owner.promise)) {
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
                    !n->owner.ops->cancel_requested(n->owner.promise)) {
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
