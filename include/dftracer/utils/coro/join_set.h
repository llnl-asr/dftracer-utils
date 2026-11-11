// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CORO_JOIN_SET_H
#define DFTRACER_UTILS_CORO_JOIN_SET_H

#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/channel.h>
#include <dftracer/utils/coro/combinators.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/time.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/vector.h>

#include <chrono>
#include <cstddef>
#include <exception>
#include <type_traits>
#include <utility>

namespace dftracer::utils::coro {

template <class T>
class JoinSet;

namespace detail {

// An outcome in the queue of a join set. One that no reader takes, as when
// the set ends first, is dropped on purpose.
template <class T>
struct QueuedOutcome {
    explicit QueuedOutcome(dstd::Expected<T, dstd::Error> value)
        : outcome(std::move(value)) {}
    QueuedOutcome(QueuedOutcome&&) = default;
    QueuedOutcome& operator=(QueuedOutcome&&) = default;
    ~QueuedOutcome() { outcome.discard(); }

    dstd::Expected<T, dstd::Error> outcome;
};

template <class T>
using SettledResults = UnboundedChannel<QueuedOutcome<T>>;

// A frame parameter, so it reports a child that never started as well as one
// whose frame a cancelled wait ended.
template <class T>
class JoinReport {
   public:
    explicit JoinReport(dstd::SharedPtr<SettledResults<T>> results) noexcept
        : results_(std::move(results)) {}
    JoinReport(JoinReport&&) noexcept = default;
    JoinReport& operator=(JoinReport&&) = delete;
    ~JoinReport() {
        if (results_) {
            results_->try_send(QueuedOutcome<T>(dstd::Unexpected(dstd::Error{
                dstd::Condition::Cancelled, "a join set task was cancelled"})));
        }
    }

    void send(dstd::Expected<T, dstd::Error> outcome) {
        results_->try_send(QueuedOutcome<T>(std::move(outcome)));
        results_.reset();
    }

   private:
    dstd::SharedPtr<SettledResults<T>> results_;
};

template <class T>
CoroTask<void> join_set_child(CoroTask<T> task, JoinReport<T> report) {
    auto settled = co_await settle(std::move(task));
    if (settled.exception) {
        report.send(
            dstd::Unexpected(common::error_of_exception(settled.exception)));
    } else if (settled.stopped) {
        // The report sends the cancelled outcome when the frame ends.
        co_await stop_here();
    } else if constexpr (std::is_void_v<T>) {
        report.send({});
    } else {
        report.send(std::move(*settled.value));
    }
}

template <class T>
CoroTask<dstd::Expected<T, dstd::Error>> settle_task(CoroTask<T> task) {
    auto settled = co_await settle(std::move(task));
    if (settled.exception) {
        co_return dstd::Unexpected(
            common::error_of_exception(settled.exception));
    }
    if (settled.stopped) {
        co_return dstd::Unexpected(
            dstd::Error{dstd::Condition::Cancelled, "a task was cancelled"});
    }
    if constexpr (std::is_void_v<T>) {
        co_return dstd::Expected<T, dstd::Error>{};
    } else {
        co_return dstd::Expected<T, dstd::Error>(std::move(*settled.value));
    }
}

}  // namespace detail

/**
 * @brief Tasks that start when added and give their results in the order they
 * finish, with errors as values.
 *
 * Made by @ref with_join_set, which owns the running tasks. One task reads
 * @ref next at a time. Results wait in a queue with no bound, so the caller
 * limits how many tasks it adds.
 */
template <class T>
class JoinSet {
   public:
    using Outcome = dstd::Optional<dstd::Expected<T, dstd::Error>>;

    explicit JoinSet(Scope& scope)
        : scope_(scope),
          results_(make_unbounded_channel<detail::QueuedOutcome<T>>()) {}

    JoinSet(const JoinSet&) = delete;
    JoinSet& operator=(const JoinSet&) = delete;
    ~JoinSet() { dstd::discard_unread(ready_); }

    /// A task that the scope refuses, or that a stop ends, gives a
    /// dstd::Condition::Cancelled error from @ref next.
    void spawn(CoroTask<T> task) {
        ++outstanding_;
        scope_.spawn(detail::join_set_child<T>(
            std::move(task), detail::JoinReport<T>(results_)));
    }

    /// The next result, or nothing when no added task is left to report.
    CoroTask<Outcome> next() {
        if (!ready_ && outstanding_ != 0) {
            auto wait = fill();
            co_await std::move(wait);
        }
        co_return take_ready();
    }

    /// @ref next that gives a dstd::Condition::Timeout error when @p wait
    /// passes first. A result that comes at that moment stays for the next
    /// read, which `with_timeout(set.next(), wait)` cannot promise.
    template <class Rep, class Period>
    CoroTask<dstd::Result<Outcome>> next_within(
        std::chrono::duration<Rep, Period> wait) {
        if (!ready_ && outstanding_ != 0) {
            auto bounded = with_timeout(fill(), wait);
            auto waited = co_await std::move(bounded);
            if (!waited) co_return dstd::Unexpected(waited.error());
        }
        co_return take_ready();
    }

    /// Tasks added and not yet read.
    std::size_t size() const noexcept { return outstanding_; }

    /// Ends every running task as cancelled at its next wait.
    void request_stop() noexcept { scope_.request_stop(); }

   private:
    CoroTask<void> fill() {
        auto outcome = co_await results_->receive();
        if (outcome) ready_ = std::move(outcome->outcome);
    }

    Outcome take_ready() {
        Outcome outcome = std::move(ready_);
        ready_.reset();
        if (outcome) --outstanding_;
        return outcome;
    }

    Scope& scope_;
    dstd::SharedPtr<detail::SettledResults<T>> results_;
    Outcome ready_;
    std::size_t outstanding_{0};
};

/**
 * @brief Runs `body(set)` with a @ref JoinSet and stops and joins the tasks it
 * left running when it ends.
 *
 * An error from `body` is raised after the join. A task's error goes only to
 * @ref JoinSet::next.
 */
template <class T, class F,
          class R = typename detail::UnwrapTask<
              std::invoke_result_t<F&, JoinSet<T>&>>::type>
CoroTask<R> with_join_set(F body) {
    auto run = [&body](Scope& scope) -> CoroTask<R> {
        JoinSet<T> set(scope);
        if constexpr (std::is_void_v<R>) {
            co_await body(set);
            scope.request_stop();
        } else {
            R value = co_await body(set);
            scope.request_stop();
            co_return value;
        }
    };
    auto task = with_scope(std::move(run));
    if constexpr (std::is_void_v<R>) {
        co_await std::move(task);
    } else {
        co_return co_await std::move(task);
    }
}

/**
 * @brief Runs @p tasks and yields each outcome as it finishes, with errors as
 * values, so one failure does not stop the others.
 *
 * Bound the wait for each outcome with @ref with_item_timeout. Dropping the
 * generator starts no more tasks. A running task ends in the background, so
 * tasks must own what they use.
 */
template <class T>
AsyncGenerator<dstd::Expected<T, dstd::Error>> as_completed(
    dstd::Vector<CoroTask<T>> tasks, ParallelOptions options = {}) {
    return parallel_map_unordered(std::move(tasks), detail::settle_task<T>,
                                  options);
}

}  // namespace dftracer::utils::coro

#endif  // DFTRACER_UTILS_CORO_JOIN_SET_H
