// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_OP_H
#define DFTRACER_UTILS_COMM_OP_H

#include <dftracer/utils/comm/detail/observe.h>
#include <dftracer/utils/coro/detail/parked.h>
#include <dftracer/utils/coro/detail/resumption_helper.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/timer_service.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/vector.h>

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

namespace dftracer::utils::comm {

/// What a call is: still in flight, or how it ended.
enum class OpStatus : std::uint8_t { Pending, Ok, Canceled, Failed };

class PendingOp;

namespace detail {

struct GroupState;

/// Aborts the group of a call that ended without its peers, and gives the
/// error the group carries afterwards.
dstd::Error cancel_group(GroupState& group, std::uint32_t rank) noexcept;

/// Aborts @p group because the call @p op was cancelled, unless that call
/// ended first: a call that ended keeps its outcome.
void cancel_group(GroupState& group, const PendingOp* op,
                  std::uint32_t rank) noexcept;

/// Aborts @p group because the awaiting rank @p rank was cancelled, through
/// @p executor, so the peers end too even when the waiter is never resumed.
void abandon_group(const dstd::SharedPtr<GroupState>& group, std::uint32_t rank,
                   coro::Executor* executor) noexcept;

/// One coroutine suspended on a @ref PendingOp.
struct OpWaiter {
    std::coroutine_handle<> handle;
    coro::Executor* executor = nullptr;
};

/// Calls that are alive now, which the tests read to prove a finished call
/// pins no state.
std::size_t count_live_ops() noexcept;

struct TimerTarget;

}  // namespace detail

/**
 * @brief The shared state of one in-flight call: it ends exactly once.
 *
 * Thread safe. A call ends with a value or with the error that ended it, and
 * an end that loses the race with the first one is dropped. Any end of the
 * call cancels the timeout that guards it, so a finished call pins nothing.
 */
class PendingOp {
   public:
    PendingOp() noexcept;
    PendingOp(const PendingOp&) = delete;
    PendingOp& operator=(const PendingOp&) = delete;
    PendingOp(PendingOp&&) = delete;
    PendingOp& operator=(PendingOp&&) = delete;
    virtual ~PendingOp();

    bool is_done() const noexcept {
        return done_.load(std::memory_order_acquire);
    }

    /// Blocks until the call ends. A worker thread of an executor gives up its
    /// run slot while it waits.
    void wait_done();

    /// Ends the call with @p why unless it already ended. Every waiter is
    /// resumed through its executor, never on this thread.
    void fail(dstd::Error why) noexcept;

    /// Ends the call with the value it staged, unless it already ended, and
    /// gives whether it ended it here.
    virtual bool commit() noexcept { return false; }

    /**
     * @brief Parks @p handle until the call ends, and registers @p awaiter
     * with @p executor so a cancel of the waiting task or a shutdown reaches
     * it.
     *
     * Gives false when the call already ended, so the caller resumes without
     * suspending. Otherwise it gives the registry entry in @p node, which the
     * caller gives back to `Executor::unpark` once the wait ends.
     */
    bool park(std::coroutine_handle<> handle, coro::Executor* executor,
              void* awaiter, coro::detail::ParkCancelFn cancel,
              coro::detail::ParkNode*& node) noexcept;

    /// Removes the waiter @p handle when the call is still pending, and gives
    /// true. False once the call ended or while another thread holds the call.
    bool withdraw(std::coroutine_handle<> handle) noexcept;

    /// Ends the call on its own, with @p why, for a call that stands alone.
    ///
    /// A call of a group has peers that already matched it, so ending one of
    /// those ends the group; a call that no group holds, such as a channel
    /// call, ends alone and leaves every other call alone. Such a call asks its
    /// provider to drop what it still holds of it, and ends through the
    /// provider, so a cancelled receive keeps its message for a later one and a
    /// call the provider already paired keeps its outcome.
    virtual void cancel_alone(const dstd::Error& why) noexcept { (void)why; }

    /// Notes the timeout that guards the call, so that any end of the call
    /// cancels it, and hands @p target to it. A call that already ended
    /// cancels the timer and empties @p target here.
    void set_timeout(coro::TimerService* timers, coro::TimerService::TimerId id,
                     dstd::SharedPtr<detail::TimerTarget> target) noexcept;

    /// Observes the call with @p meta, from @ref detail::begin_call: its end is
    /// counted and traced. Set before the call is shared with another thread.
    void observe(detail::CallMeta meta) noexcept { meta_ = std::move(meta); }

    /// Sets the sequence number of the call, under the lock that registers it.
    void set_seq(std::uint64_t seq) noexcept { meta_.seq = seq; }

    /// Notes when the call began, on the steady clock, for a call that has a
    /// deadline and so reads the clock anyway. Before the call is shared.
    void set_start_ns(std::uint64_t now) noexcept {
        if (meta_.tally && meta_.start_ns == 0) meta_.start_ns = now;
    }

    /// What the call knows of itself; the tally is empty for a call nothing
    /// observes.
    const detail::CallMeta& meta() const noexcept { return meta_; }

    /// The bytes this call counts when it ends well.
    std::uint64_t sent() const noexcept {
        return bytes_sent_.load(std::memory_order_relaxed);
    }
    std::uint64_t received() const noexcept {
        return bytes_received_.load(std::memory_order_relaxed);
    }

    /// Sets the bytes this call counts when it ends well; once, before it ends.
    void set_sent(std::uint64_t bytes) noexcept {
        bytes_sent_.store(bytes, std::memory_order_relaxed);
    }
    void set_received(std::uint64_t bytes) noexcept {
        bytes_received_.store(bytes, std::memory_order_relaxed);
    }

   protected:
    /// Runs @p set and ends the call, unless it already ended. Returns whether
    /// it ended the call here. The waiters resume after the lock is released.
    template <class F>
    bool settle(F&& set) noexcept {
        dstd::Vector<detail::OpWaiter> waiters;
        detail::EndKind end = detail::EndKind::Ok;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (done_.load(std::memory_order_relaxed)) return false;
            set();
            if (error_)
                end = error_->is(dstd::Condition::Cancelled)
                          ? detail::EndKind::Cancelled
                          : detail::EndKind::Failed;
            // Counted and traced before anyone can see the call done, so a
            // waiter that reads the counters finds this call in them.
            if (meta_.tally && (end != detail::EndKind::Ok || !meta_.batched))
                note_end(end);
            done_.store(true, std::memory_order_release);
            waiters = std::move(waiters_);
        }
        cv_.notify_all();
        for (const detail::OpWaiter& waiter : waiters)
            coro::detail::post_resumption(waiter.executor, waiter.handle);
        end_timeout();
        return true;
    }

    /// Read only once the call ended.
    const dstd::Optional<dstd::Error>& failure() const noexcept {
        return error_;
    }

    dstd::Optional<dstd::Error> error_;

    void end_timeout() noexcept;

    void note_end(detail::EndKind end) noexcept;

   private:
    detail::CallMeta meta_;
    std::atomic<std::uint64_t> bytes_sent_{0};
    std::atomic<std::uint64_t> bytes_received_{0};
    std::mutex mutex_;
    std::condition_variable cv_;
    dstd::Vector<detail::OpWaiter> waiters_;
    coro::TimerService* timers_ = nullptr;
    coro::TimerService::TimerId timer_ = coro::TimerService::NO_TIMER;
    dstd::SharedPtr<detail::TimerTarget> timer_target_;
    std::atomic<bool> done_{false};
};

/// A call that gives a value of @p T.
template <class T>
class OpState : public PendingOp {
   public:
    bool succeed(T value) noexcept {
        return this->settle(
            [this, &value] { value_.emplace(std::move(value)); });
    }

    /// Keeps @p value for @ref commit, so the ranks of one slot all end with a
    /// value or none does. The call stays pending.
    void stage(T value) noexcept { value_.emplace(std::move(value)); }

    bool commit() noexcept override {
        return this->settle([] {});
    }

    /// The outcome; read only once the call ended. Every read gives a copy.
    dstd::Result<T> take() const {
        if (this->failure())
            return dstd::Unexpected<dstd::Error>(*this->failure());
        return dstd::Result<T>(*value_);
    }

   private:
    dstd::Optional<T> value_;
};

template <>
class OpState<void> : public PendingOp {
   public:
    bool succeed() noexcept {
        return this->settle([] {});
    }

    dstd::Result<void> take() const {
        if (this->failure())
            return dstd::Unexpected<dstd::Error>(*this->failure());
        return {};
    }
};

/**
 * @brief A handle to one in-flight call.
 *
 * It ends exactly once: with a value, or with the error that ended it, which
 * is a cancellation of the call, a passed deadline or the abort of its group.
 * Copies refer to the same call and read the same outcome. @ref test asks,
 * @ref wait waits, and `co_await` waits without holding a thread. Reading the
 * outcome of one call from two threads at once is not allowed.
 *
 * A `co_await` parks with the executor of the waiting task, so a cancel of that
 * task or a shutdown of the executor ends the wait as cancelled, and the group
 * is aborted with it: the peers have already matched the call.
 *
 * Made by @ref Group; a user never makes one.
 */
template <class T>
class Op {
   public:
    Op(dstd::SharedPtr<OpState<T>> state,
       dstd::SharedPtr<detail::GroupState> group, std::uint32_t rank) noexcept
        : state_(std::move(state)), group_(std::move(group)), rank_(rank) {}
    /// What one suspension of a handle remembers, which is not part of its
    /// value and is never copied with it.
    struct AwaitState {
        std::coroutine_handle<> handle;
        coro::Executor* executor = nullptr;
        coro::detail::ParkNode* park = nullptr;
        bool cancelled = false;
    };

    Op(const Op& other) noexcept
        : state_(other.state_), group_(other.group_), rank_(other.rank_) {}
    Op(Op&& other) noexcept
        : state_(std::move(other.state_)),
          group_(std::move(other.group_)),
          rank_(other.rank_) {}
    Op& operator=(const Op& other) noexcept {
        state_ = other.state_;
        group_ = other.group_;
        rank_ = other.rank_;
        return *this;
    }
    Op& operator=(Op&& other) noexcept {
        state_ = std::move(other.state_);
        group_ = std::move(other.group_);
        rank_ = other.rank_;
        return *this;
    }
    ~Op() = default;

    /// True once the call ended.
    bool test() const noexcept { return state_->is_done(); }

    /// How the call stands: @ref OpStatus::Pending until it ends.
    OpStatus status() const noexcept {
        if (!state_->is_done()) return OpStatus::Pending;
        const dstd::Result<T> outcome = state_->take();
        if (outcome) return OpStatus::Ok;
        return outcome.error().is(dstd::Condition::Cancelled)
                   ? OpStatus::Canceled
                   : OpStatus::Failed;
    }

    /// Waits for the call and gives its outcome.
    dstd::Result<T> wait() {
        state_->wait_done();
        return state_->take();
    }

    /**
     * @brief Cancels the call and returns at once.
     *
     * The peers of a call of a group have already matched it, so a call still
     * in flight aborts the group and every member ends with the same cancelled
     * error. A call that no group holds, such as a channel call, ends alone and
     * leaves the group of the others alone: its provider ends it with the
     * cancel when it still holds the call, and a call it already paired keeps
     * its outcome, which arrives through the provider, so wait for it with @ref
     * wait after a cancel. A call that ended before the cancel keeps its
     * outcome.
     */
    void cancel() noexcept {
        if (!group_) {
            state_->cancel_alone(
                dstd::Error{dstd::Condition::Cancelled, "canceled a call"});
            return;
        }
        detail::cancel_group(*group_, state_.get(), rank_);
    }

    bool await_ready() const noexcept { return state_->is_done(); }

    template <class Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        await_.handle = handle;
        await_.executor = coro::detail::executor_for(handle);
        return state_->park(handle, await_.executor, this, &Op::cancel_thunk,
                            await_.park);
    }

    dstd::Result<T> await_resume() {
        if (await_.park != nullptr) {
            await_.executor->unpark(await_.park);
            await_.park = nullptr;
        }
        if (!await_.cancelled) return state_->take();
        if (!group_) {
            state_->wait_done();
            return state_->take();
        }
        return dstd::Unexpected<dstd::Error>(
            detail::cancel_group(*group_, rank_));
    }

   private:
    static void cancel_thunk(void* self) noexcept {
        static_cast<Op*>(self)->cancel_await();
    }

    /// The waiting task was cancelled: it leaves the wait, so it resumes once,
    /// and the group is aborted for the peers that already matched the call.
    void cancel_await() noexcept {
        if (await_.executor == nullptr) return;
        if (!state_->withdraw(await_.handle)) return;
        await_.cancelled = true;
        if (group_) {
            detail::abandon_group(group_, rank_, await_.executor);
        } else {
            state_->cancel_alone(
                dstd::Error{dstd::Condition::Cancelled, "canceled a call"});
        }
        coro::detail::stop_or_resume(await_.executor, await_.park,
                                     await_.handle);
    }

    dstd::SharedPtr<OpState<T>> state_;
    dstd::SharedPtr<detail::GroupState> group_;
    std::uint32_t rank_ = 0;
    mutable AwaitState await_{};
};

}  // namespace dftracer::utils::comm

#endif  // DFTRACER_UTILS_COMM_OP_H
