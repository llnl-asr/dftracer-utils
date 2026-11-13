// SPDX-License-Identifier: MIT
#include <dftracer/utils/coro/combinators.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/time.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi_context.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/plugins/host/fold_adapter/ext.h>
#include <dftracer/utils/plugins/task_abi.h>
#include <dftracer/utils/runtime/runtime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

namespace dftracer::utils::plugins::host {

namespace {

constexpr std::uint32_t DRIVE_WAIT_CAP = 64;

PluginFold& fold_of(void* h) { return *static_cast<PluginFold*>(h); }

bool is_unawaited(::dftu_task* task) {
    return task != nullptr && !plugins::abi_task(task)->awaited;
}

// Awaits a task handed back to the host, takes its message on failure and
// frees it.
coro::CoroTask<dftu_status> consume(::dftu_task* handed,
                                    dstd::String* message) {
    plugins::AbiTask* task = plugins::abi_task(handed);
    const dftu_status status = co_await plugins::run_abi_task(task);
    if (status < 0) *message = std::move(task->message);
    plugins::free_abi_task(handed);
    co_return status;
}

Job* job_of(::dftu_job* j) { return reinterpret_cast<Job*>(j); }

::dftu_job* as_abi(Job& j) { return reinterpret_cast<::dftu_job*>(&j); }

dftu_status fail(::dftu_ctx* ctx, dftu_status status, const char* message) {
    return ctx != nullptr ? dftu_fail(ctx, status, message) : status;
}

void take_message(const plugins::AbiContext& ctx, const char* what,
                  dstd::String* out) {
    const dstd::StringView text = ctx.message();
    if (text.empty()) {
        out->assign(what);
    } else {
        out->assign(text.data(), text.size());
    }
}

coro::CoroTask<dftu_status> eval(PluginFold* fold, Job* job, const void* in,
                                 void* out, dstd::String* message);

coro::CoroTask<dftu_status> run_leaf(Job* job, const void* in, void* out,
                                     dstd::String* message) {
    if (job->where != DFTU_JOB_INLINE) co_await coro::yield();
    const coro::CancelView cancel = co_await coro::this_coro::cancel_view;
    if (cancel.requested()) {
        *message = "cancelled";
        co_return DFTU_E_CANCELLED;
    }
    plugins::AbiContext ctx(cancel);
    ::dftu_task* pending = nullptr;
    dftu_status status = DFTU_OK;
    if (job->where == DFTU_JOB_BLOCKING) {
        coro::BlockingRegion block;
        status = job->fn(ctx.get(), job->state, in, out, &pending);
    } else {
        status = job->fn(ctx.get(), job->state, in, out, &pending);
    }
    if (status < 0) {
        take_message(ctx, "job failed", message);
        co_return status;
    }
    if (pending == nullptr) co_return status;
    if (!is_unawaited(pending)) {
        *message = "job: pending is a task that was already awaited";
        co_return DFTU_E_INVALID;
    }
    status = co_await consume(pending, message);
    co_return status;
}

coro::CoroTask<dftu_status> run_sleep(Job* job, dstd::String* message) {
    co_await coro::sleep(std::chrono::nanoseconds(job->sleep_ns));
    *message = "deadline passed";
    co_return DFTU_E_TIMEOUT;
}

coro::CoroTask<dftu_status> run_then(PluginFold* fold, Job* job, const void* in,
                                     void* out, dstd::String* message) {
    Job* first = job->children[0];
    Job* second = job->children[1];
    dstd::Vector<char> mid(first->out_size);
    const dftu_status status =
        co_await eval(fold, first, in, mid.data(), message);
    if (status < 0) co_return status;
    const dftu_status next =
        co_await eval(fold, second, mid.data(), out, message);
    co_return next;
}

// The outcome a fan-out keeps: for `all` and `map` the first error, for `any`
// the first job to finish.
struct FanState {
    std::atomic<bool> decided{false};
    dftu_status status{DFTU_OK};
    dstd::String message;
};

void decide(FanState* state, coro::Scope* scope, dftu_status status,
            dstd::String message) {
    if (state->decided.exchange(true, std::memory_order_acq_rel)) return;
    state->status = status;
    state->message = std::move(message);
    scope->request_stop();
}

coro::CoroTask<void> all_child(PluginFold* fold, Job* job, const void* in,
                               void* out, FanState* state, coro::Scope* scope) {
    dstd::String message;
    const dftu_status status = co_await eval(fold, job, in, out, &message);
    if (status < 0) decide(state, scope, status, std::move(message));
}

coro::CoroTask<void> any_child(PluginFold* fold, Job* job, const void* in,
                               char* buffer, void* out, std::uint32_t size,
                               FanState* state, coro::Scope* scope) {
    dstd::String message;
    const dftu_status status = co_await eval(fold, job, in, buffer, &message);
    if (state->decided.exchange(true, std::memory_order_acq_rel)) co_return;
    state->status = status;
    state->message = std::move(message);
    if (status >= 0 && size != 0) std::memcpy(out, buffer, size);
    scope->request_stop();
}

struct MapState {
    std::atomic<std::uint64_t> next{0};
    const char* items{nullptr};
    char* slots{nullptr};
    std::uint64_t count{0};
};

coro::CoroTask<void> map_worker(PluginFold* fold, Job* body, MapState* map,
                                FanState* state, coro::Scope* scope) {
    for (;;) {
        const std::uint64_t i =
            map->next.fetch_add(1, std::memory_order_relaxed);
        if (i >= map->count || state->decided.load(std::memory_order_acquire)) {
            co_return;
        }
        dstd::String message;
        const dftu_status status =
            co_await eval(fold, body, map->items + i * body->in_size,
                          map->slots + i * body->out_size, &message);
        if (status < 0) {
            decide(state, scope, status, std::move(message));
            co_return;
        }
    }
}

// The fan-out result once the scope joined: a cancel of this job wins over
// what the children decided, since their outputs may be partial.
coro::CoroTask<dftu_status> fan_result(FanState* state, dstd::String* message) {
    const bool cancelled = co_await coro::this_coro::stop_requested;
    if (cancelled) {
        *message = "cancelled";
        co_return DFTU_E_CANCELLED;
    }
    if (state->status < 0) *message = std::move(state->message);
    co_return state->status;
}

coro::CoroTask<dftu_status> run_fan(PluginFold* fold, Job* job, const void* in,
                                    void* out, dstd::String* message) {
    const coro::PromiseBase* owner = co_await coro::detail::OWNS_SCOPE;
    coro::Executor* executor = co_await coro::this_coro::executor;
    FanState state;
    MapState map;
    dstd::Vector<dstd::Vector<char>> buffers;
    coro::Scope scope(executor, owner);
    if (job->kind == Job::Kind::All) {
        std::size_t offset = 0;
        for (Job* child : job->children) {
            scope.spawn(all_child(fold, child, in,
                                  static_cast<char*>(out) + offset, &state,
                                  &scope));
            offset += child->out_size;
        }
    } else if (job->kind == Job::Kind::Any) {
        state.status = DFTU_E_UNKNOWN;
        buffers.resize(job->children.size());
        for (std::size_t i = 0; i < job->children.size(); ++i) {
            buffers[i].resize(job->out_size);
            scope.spawn(any_child(fold, job->children[i], in, buffers[i].data(),
                                  out, job->out_size, &state, &scope));
        }
    } else {
        const auto* items = static_cast<const ::dftu_job_items*>(in);
        auto* slots = static_cast<::dftu_job_slots*>(out);
        if (items->n != slots->n) {
            *message = "map: the output has room for another count of items";
            co_return DFTU_E_INVALID;
        }
        Job* body = job->children[0];
        map.items = static_cast<const char*>(items->items);
        map.slots = static_cast<char*>(slots->items);
        map.count = items->n;
        const std::size_t workers = coro::detail::parallel_worker_count(
            job->max_parallel, static_cast<std::size_t>(items->n), executor);
        for (std::size_t i = 0; i < workers; ++i) {
            scope.spawn(map_worker(fold, body, &map, &state, &scope));
        }
    }
    co_await scope.join_quietly();
    const dftu_status status = co_await fan_result(&state, message);
    co_return status;
}

coro::CoroTask<dftu_status> eval(PluginFold* fold, Job* job, const void* in,
                                 void* out, dstd::String* message) {
    dftu_status status = DFTU_OK;
    switch (job->kind) {
        case Job::Kind::Leaf:
            status = co_await run_leaf(job, in, out, message);
            break;
        case Job::Kind::Sleep:
            status = co_await run_sleep(job, message);
            break;
        case Job::Kind::Then:
            status = co_await run_then(fold, job, in, out, message);
            break;
        case Job::Kind::All:
        case Job::Kind::Any:
        case Job::Kind::Map:
            status = co_await run_fan(fold, job, in, out, message);
            break;
    }
    co_return status;
}

coro::CoroTask<void> run_job(PluginFold* fold, Job* job, const void* in,
                             void* out, plugins::AbiTask* task) {
    dstd::String message;
    const dftu_status status = co_await eval(fold, job, in, out, &message);
    if (status < 0) task->fail(status, message);
}

coro::CoroTask<void> run_spawn(dftu_status (*fn)(::dftu_ctx*, void*), void* arg,
                               ::dftu_job_where where, plugins::AbiTask* task) {
    if (where != DFTU_JOB_INLINE) co_await coro::yield();
    const coro::CancelView cancel = co_await coro::this_coro::cancel_view;
    plugins::AbiContext ctx(cancel);
    dftu_status status = DFTU_OK;
    if (where == DFTU_JOB_BLOCKING) {
        coro::BlockingRegion block;
        status = fn(ctx.get(), arg);
    } else {
        status = fn(ctx.get(), arg);
    }
    if (status < 0) {
        dstd::String message;
        take_message(ctx, "spawned function failed", &message);
        task->fail(status, message);
    }
}

coro::CoroTask<void> await_one(::dftu_task* task) {
    co_await plugins::run_abi_task(plugins::abi_task(task));
}

coro::CoroTask<void> await_round(::dftu_task* const* tasks, std::uint32_t n) {
    if (n == 1) {
        co_await await_one(tasks[0]);
        co_return;
    }
    const coro::PromiseBase* owner = co_await coro::detail::OWNS_SCOPE;
    coro::Executor* executor = co_await coro::this_coro::executor;
    coro::Scope scope(executor, owner);
    for (std::uint32_t i = 0; i < n; ++i) scope.spawn(await_one(tasks[i]));
    co_await scope.join_quietly();
}

// Owns the tasks of one round; frees them on every exit, a stop included.
struct Round {
    ::dftu_task* tasks[DRIVE_WAIT_CAP] = {};
    std::uint32_t n = 0;

    Round() = default;
    Round(const Round&) = delete;
    Round& operator=(const Round&) = delete;
    ~Round() { clear(); }

    void clear() {
        for (std::uint32_t i = 0; i < n; ++i) {
            if (tasks[i]) plugins::free_abi_task(tasks[i]);
        }
        n = 0;
    }
};

// The plugin's drive state; released once when the drive frame dies, which
// also covers a drive task freed before it ran.
struct DriveState {
    void* coro = nullptr;
    void (*release)(void*) = nullptr;

    DriveState(void* c, void (*r)(void*)) : coro(c), release(r) {}
    DriveState(DriveState&& other) noexcept
        : coro(other.coro), release(other.release) {
        other.coro = nullptr;
    }
    DriveState(const DriveState&) = delete;
    DriveState& operator=(const DriveState&) = delete;
    DriveState& operator=(DriveState&&) = delete;
    ~DriveState() {
        if (coro != nullptr && release != nullptr) release(coro);
    }
};

// The tasks of a round stay readable through status() during the next step,
// then are freed.
coro::CoroTask<void> run_drive(::dftu_step_fn step, DriveState state,
                               plugins::AbiTask* task) {
    Round wait;
    Round done;
    for (;;) {
        const coro::CancelView cancel = co_await coro::this_coro::cancel_view;
        if (cancel.requested()) {
            task->fail(DFTU_E_CANCELLED, "drive: cancelled");
            co_return;
        }
        plugins::AbiContext ctx(cancel);
        std::uint32_t n = 0;
        const dftu_status status =
            step(ctx.get(), state.coro, wait.tasks, DRIVE_WAIT_CAP, &n);
        done.clear();
        wait.n = n > DRIVE_WAIT_CAP ? DRIVE_WAIT_CAP : n;
        if (status == DFTU_END) co_return;
        if (status < 0) {
            dstd::String message;
            take_message(ctx, "drive step failed", &message);
            task->fail(status, message);
            co_return;
        }
        if (n == 0 || n > DRIVE_WAIT_CAP) {
            task->fail(DFTU_E_INVALID,
                       "drive step: returned no tasks to wait for, or more "
                       "than the room it was given");
            co_return;
        }
        bool valid = true;
        for (std::uint32_t i = 0; i < n; ++i)
            valid = valid && is_unawaited(wait.tasks[i]);
        if (!valid) {
            task->fail(DFTU_E_INVALID,
                       "drive step: a task is NULL or already awaited");
            co_return;
        }
        co_await await_round(wait.tasks, n);
        for (std::uint32_t i = 0; i < n; ++i) {
            done.tasks[i] = wait.tasks[i];
            wait.tasks[i] = nullptr;
        }
        done.n = n;
        wait.n = 0;
    }
}

dftu_status host_job(::dftu_ctx* ctx, void* h, const ::dftu_job_desc* desc,
                     ::dftu_job** out) {
    if (desc == nullptr || out == nullptr || desc->fn == nullptr) {
        return fail(ctx, DFTU_E_INVALID, "job: NULL descriptor, out or fn");
    }
    if (desc->struct_size != sizeof(::dftu_job_desc)) {
        return fail(ctx, DFTU_E_INVALID, "job: struct_size is not sizeof");
    }
    if (desc->where < DFTU_JOB_CPU || desc->where > DFTU_JOB_BLOCKING) {
        return fail(ctx, DFTU_E_INVALID, "job: unknown where");
    }
    Job& job = fold_of(h).new_job(Job::Kind::Leaf);
    job.where = desc->where;
    job.fn = desc->fn;
    job.state = desc->state;
    job.free_state = desc->free_state;
    job.in_ty = desc->in_ty;
    job.in_size = desc->in_size;
    job.out_ty = desc->out_ty;
    job.out_size = desc->out_size;
    *out = as_abi(job);
    return DFTU_OK;
}

dftu_status host_sleep(::dftu_ctx* ctx, void* h, std::uint64_t ns,
                       ::dftu_job** out) {
    if (out == nullptr) return fail(ctx, DFTU_E_INVALID, "sleep: NULL out");
    Job& job = fold_of(h).new_job(Job::Kind::Sleep);
    job.sleep_ns = ns;
    *out = as_abi(job);
    return DFTU_OK;
}

dftu_status host_then(::dftu_ctx* ctx, void* h, ::dftu_job* a, ::dftu_job* b,
                      ::dftu_job** out) {
    if (a == nullptr || b == nullptr || out == nullptr) {
        return fail(ctx, DFTU_E_INVALID, "then: NULL job or out");
    }
    Job* first = job_of(a);
    Job* second = job_of(b);
    if (first->out_ty != second->in_ty || first->out_size != second->in_size) {
        return fail(ctx, DFTU_E_INVALID,
                    "then: the output of the first job is not the input of "
                    "the second (type or size)");
    }
    Job& job = fold_of(h).new_job(Job::Kind::Then);
    job.in_ty = first->in_ty;
    job.in_size = first->in_size;
    job.out_ty = second->out_ty;
    job.out_size = second->out_size;
    job.children = {first, second};
    *out = as_abi(job);
    return DFTU_OK;
}

dftu_status fan(::dftu_ctx* ctx, void* h, ::dftu_job* const* jobs,
                std::uint32_t n, Job::Kind kind, ::dftu_job** out) {
    if (jobs == nullptr || n == 0 || out == nullptr) {
        return fail(ctx, DFTU_E_INVALID,
                    kind == Job::Kind::All ? "all: no jobs or NULL out"
                                           : "any: no jobs or NULL out");
    }
    Job* first = job_of(jobs[0]);
    dstd::Vector<Job*> children;
    children.reserve(n);
    std::uint32_t out_total = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        Job* child = jobs[i] != nullptr ? job_of(jobs[i]) : nullptr;
        const bool same_in = child != nullptr && child->in_ty == first->in_ty &&
                             child->in_size == first->in_size;
        const bool same_out = child != nullptr &&
                              child->out_ty == first->out_ty &&
                              child->out_size == first->out_size;
        if (!same_in || (kind == Job::Kind::Any && !same_out)) {
            return fail(ctx, DFTU_E_INVALID,
                        kind == Job::Kind::All
                            ? "all: the jobs do not share one input type and "
                              "size"
                            : "any: the jobs do not share one input and one "
                              "output type and size");
        }
        children.push_back(child);
        out_total += child->out_size;
    }
    Job& job = fold_of(h).new_job(kind);
    job.in_ty = first->in_ty;
    job.in_size = first->in_size;
    job.out_ty = kind == Job::Kind::All ? DFTU_T_BYTES : first->out_ty;
    job.out_size = kind == Job::Kind::All ? out_total : first->out_size;
    job.children = std::move(children);
    *out = as_abi(job);
    return DFTU_OK;
}

dftu_status host_all(::dftu_ctx* ctx, void* h, ::dftu_job* const* jobs,
                     std::uint32_t n, ::dftu_job** out) {
    return fan(ctx, h, jobs, n, Job::Kind::All, out);
}

dftu_status host_any(::dftu_ctx* ctx, void* h, ::dftu_job* const* jobs,
                     std::uint32_t n, ::dftu_job** out) {
    return fan(ctx, h, jobs, n, Job::Kind::Any, out);
}

dftu_status host_map(::dftu_ctx* ctx, void* h, ::dftu_job* body,
                     std::uint32_t max_parallel, ::dftu_job** out) {
    if (body == nullptr || out == nullptr) {
        return fail(ctx, DFTU_E_INVALID, "map: NULL body or out");
    }
    Job& job = fold_of(h).new_job(Job::Kind::Map);
    job.in_ty = DFTU_T_BYTES;
    job.in_size = sizeof(::dftu_job_items);
    job.out_ty = DFTU_T_BYTES;
    job.out_size = sizeof(::dftu_job_slots);
    job.max_parallel = max_parallel;
    job.children = {job_of(body)};
    *out = as_abi(job);
    return DFTU_OK;
}

dftu_status host_start(::dftu_ctx* ctx, void* h, ::dftu_job* job,
                       const void* in, void* out, ::dftu_task** out_task) {
    if (job == nullptr || out_task == nullptr) {
        return fail(ctx, DFTU_E_INVALID, "start: NULL job or out_task");
    }
    Job* j = job_of(job);
    if ((j->in_size != 0 && in == nullptr) ||
        (j->out_size != 0 && out == nullptr)) {
        return fail(ctx, DFTU_E_INVALID,
                    "start: NULL in or out for a sized "
                    "value");
    }
    PluginFold* fold = &fold_of(h);
    *out_task =
        plugins::make_abi_task([fold, j, in, out](plugins::AbiTask* task) {
            return run_job(fold, j, in, out, task);
        });
    return DFTU_OK;
}

dftu_status host_spawn(::dftu_ctx* ctx, void* h,
                       dftu_status (*fn)(::dftu_ctx*, void*), void* arg,
                       ::dftu_job_where where, ::dftu_task** out_task) {
    if (fn == nullptr || out_task == nullptr) {
        return fail(ctx, DFTU_E_INVALID, "spawn: NULL fn or out_task");
    }
    if (where < DFTU_JOB_CPU || where > DFTU_JOB_BLOCKING) {
        return fail(ctx, DFTU_E_INVALID, "spawn: unknown where");
    }
    (void)h;
    *out_task =
        plugins::make_abi_task([fn, arg, where](plugins::AbiTask* task) {
            return run_spawn(fn, arg, where, task);
        });
    return DFTU_OK;
}

dftu_status host_drive(::dftu_ctx* ctx, void* h, ::dftu_step_fn step,
                       void* state, void (*release)(void*),
                       ::dftu_task** out_task) {
    if (step == nullptr || out_task == nullptr) {
        return fail(ctx, DFTU_E_INVALID, "drive: NULL step or out_task");
    }
    (void)h;
    *out_task =
        plugins::make_abi_task([step, state, release](plugins::AbiTask* task) {
            return run_drive(step, DriveState(state, release), task);
        });
    return DFTU_OK;
}

dftu_status host_status(::dftu_ctx* ctx, void* /*h*/, const ::dftu_task* t) {
    if (t == nullptr) return fail(ctx, DFTU_E_INVALID, "status: NULL task");
    const plugins::AbiTask* task =
        plugins::abi_task(const_cast<::dftu_task*>(t));
    if (!task->finished) {
        return fail(ctx, DFTU_E_INVALID, "status: the task has not finished");
    }
    if (task->status < 0 && ctx != nullptr) {
        return ctx->vt->set_error(
            ctx, task->status, task->message.data(),
            static_cast<std::uint32_t>(task->message.size()));
    }
    return task->status;
}

void host_discard(void* /*h*/, ::dftu_task* t) {
    if (is_unawaited(t)) plugins::free_abi_task(t);
}

}  // namespace

dftu_status detail::task_run(::dftu_ctx* ctx, void* /*h*/, ::dftu_task* t) {
    if (t == nullptr) return fail(ctx, DFTU_E_INVALID, "run: NULL task");
    plugins::AbiTask* task = plugins::abi_task(t);
    const auto ran = default_runtime().block_on(
        coro::named("dftu_svc_task::run", plugins::run_abi_task(task)));
    dftu_status status = DFTU_OK;
    dstd::Optional<dstd::Error> error;
    if (!ran) {
        error = ran.error();
    } else if (task->status < 0) {
        error = plugins::abi_task_error(*task);
    } else {
        status = task->status;
    }
    if (error) {
        const dstd::StringView text =
            error->message().empty() ? error->name() : error->message();
        status = host_fail(ctx, common::to_status(error->condition()), text);
    }
    plugins::free_abi_task(t);
    return status;
}

namespace {

const ::dftu_svc_task TASK_VTABLE = {
    sizeof(::dftu_svc_task),
    host_job,
    host_sleep,
    host_then,
    host_all,
    host_any,
    host_map,
    host_start,
    host_spawn,
    host_drive,
    host_status,
    host_discard,
    detail::task_run,
};

}  // namespace

const void* detail::task_ext_vtable() { return &TASK_VTABLE; }

}  // namespace dftracer::utils::plugins::host
