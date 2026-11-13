// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_RUNTIME_TASK_HANDLE_H
#define DFTRACER_UTILS_PYTHON_RUNTIME_TASK_HANDLE_H

#include <Python.h>
#include <dftracer/utils/common/typedefs.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/stop_token.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace dftracer::utils::python::runtime {

/// The type-erased completion of a native task, shared by the Python handle
/// and the task that fills it. The task stores its result here and marks the
/// state done on the worker that ran it, so a handle dropped before the end
/// only drops its reference and the task finishes on its own. Nothing waits on
/// a thread: blocking calls wait on a condition variable and done callbacks run
/// on the worker that finished the task.
class AsyncState {
   public:
    virtual ~AsyncState() = default;

    /// Blocks until the task ends. Call it with the GIL released.
    void wait();
    bool done() const { return done_.load(std::memory_order_acquire); }
    /// Converts the finished task's result, or sets the Python exception and
    /// gives null. Call it with the GIL held, after wait().
    virtual PyObject *to_python() = 0;

    /// Asks the task to stop. It then ends with a Cancelled error.
    void cancel() { stop_.request_stop(runtime_->executor()); }

    /// Queues `fn` to be called as `fn(handle)` when the task ends, on the
    /// pool worker that finishes it, which takes the GIL for the call. A slow
    /// callback holds that worker. False when the task has ended, and the
    /// caller runs `fn` itself. Call it with the GIL held.
    bool add_callback(PyObject *handle, PyObject *fn);

    const dstd::String &name() const { return name_; }
    common::TaskIndex id() const { return id_.load(std::memory_order_relaxed); }
    void set_id(common::TaskIndex id) {
        id_.store(id, std::memory_order_relaxed);
    }
    const coro::StopSource &stop() const { return stop_; }

    /// Ends the state with `error` when the runtime refused the task.
    virtual void refuse(dstd::Error error) = 0;

   protected:
    AsyncState(dstd::String name, coro::StopSource stop,
               dstd::SharedPtr<Runtime> runtime)
        : name_(std::move(name)),
          stop_(std::move(stop)),
          runtime_(std::move(runtime)) {}

    /// Marks the state done, wakes the waiters and runs the callbacks. Called
    /// once, after the result is stored.
    void finish();

   private:
    dstd::String name_;
    std::atomic<common::TaskIndex> id_{-1};
    coro::StopSource stop_;
    dstd::SharedPtr<Runtime> runtime_;

    std::mutex mutex_;
    std::condition_variable ended_;
    std::atomic<bool> done_{false};
    dstd::Vector<PyObject *> callbacks_;
    PyObject *owner_{nullptr};
};

/// The state of a task that gives a @ref coro::TaskResult of `T`. `Convert` is
/// called as `PyObject *(coro::TaskResult<T> &)` with the GIL held; it may move
/// the value out, and it sets the Python exception for an error result.
template <typename T, typename Convert>
class TypedAsyncState final : public AsyncState {
   public:
    TypedAsyncState(dstd::String name, Convert convert, coro::StopSource stop,
                    dstd::SharedPtr<Runtime> runtime)
        : AsyncState(std::move(name), std::move(stop), std::move(runtime)),
          convert_(std::move(convert)) {}

    PyObject *to_python() override { return convert_(*result_); }

    void complete(coro::TaskResult<T> result) {
        result_.emplace(std::move(result));
        result_->discard();
        finish();
    }

    void refuse(dstd::Error error) override {
        if (!done())
            complete(coro::TaskResult<T>(
                dstd::Unexpected<dstd::Error>(std::move(error))));
    }

   private:
    Convert convert_;
    dstd::Optional<coro::TaskResult<T>> result_;
};

/// Ends `state` as cancelled when it goes out of scope before the state ended.
template <typename State>
struct StateGuard {
    dstd::SharedPtr<State> state;
    StateGuard(dstd::SharedPtr<State> s) : state(std::move(s)) {}
    StateGuard(const StateGuard &) = delete;
    StateGuard &operator=(const StateGuard &) = delete;
    ~StateGuard() {
        state->refuse(dstd::Error{dstd::Condition::Cancelled,
                                  "task dropped before it ended"});
    }
};

/// Runs `task` linked to `token` and stores how it ended in `state`. It never
/// throws, so an error or a stop reaches the state as a value.
template <typename T, typename Convert>
coro::CoroTask<void> run_into_state(
    dstd::SharedPtr<TypedAsyncState<T, Convert>> state, coro::CoroTask<T> task,
    coro::StopToken token) {
    // A frame destroyed before it ends, as at runtime shutdown, still ends the
    // state, so no get() waits for ever.
    StateGuard<TypedAsyncState<T, Convert>> guard{state};
    auto linked = coro::with_stop_token(std::move(task), std::move(token));
    auto settled = co_await coro::settle(std::move(linked));
    if (settled.error) {
        state->complete(coro::TaskResult<T>(
            dstd::Unexpected<dstd::Error>(std::move(*settled.error))));
    } else if (settled.stopped) {
        state->complete(coro::TaskResult<T>(
            dstd::Unexpected<dstd::Error>(coro::detail::stopped_error())));
    } else {
        state->complete(coro::TaskResult<T>(std::move(*settled.value)));
    }
}

/// A new Python TaskHandle over `state`, or null with a Python error set.
PyObject *new_task_handle(dstd::SharedPtr<AsyncState> state);

/// Starts `task` on `runtime` and returns a Python TaskHandle that converts its
/// result with `convert`. The task owns its inputs; the handle can be dropped
/// before it ends.
template <typename T, typename Convert>
PyObject *submit_task_handle(dstd::SharedPtr<Runtime> runtime,
                             coro::CoroTask<T> task, dstd::StringView name,
                             Convert convert) {
    coro::StopSource stop;
    auto state = dstd::make_shared<TypedAsyncState<T, Convert>>(
        dstd::String(name), std::move(convert), stop, runtime);
    auto submitted = runtime->submit(
        run_into_state<T, Convert>(state, std::move(task), stop.token()), name);
    state->set_id(submitted.id);
    if (submitted.done()) {
        auto ended = submitted.future.get();
        if (!ended) state->refuse(ended.error());
    }
    return new_task_handle(std::move(state));
}

int init_task_handle(PyObject *m);

}  // namespace dftracer::utils::python::runtime

#endif  // DFTRACER_UTILS_PYTHON_RUNTIME_TASK_HANDLE_H
