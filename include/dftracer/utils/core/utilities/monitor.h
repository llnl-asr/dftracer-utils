#ifndef DFTRACER_UTILS_CORE_UTILITIES_MONITOR_H
#define DFTRACER_UTILS_CORE_UTILITIES_MONITOR_H

namespace dftracer::utils::utilities {

// How a coroutine first entered the monitor, shown as a tag in the report.
//   Task  - a submitted task (Executor::enqueue with a task id)
//   Spawn - scope.spawn() fan-out work
//   Io    - first seen suspending on async I/O (reached by symmetric transfer)
//   Sync  - a co_await'd CoroTask captured in deep mode
enum class CoroKind : unsigned char { Task, Spawn, Io, Sync };

namespace detail {
extern bool g_monitor_deep;
}

// Deep mode (DFTRACER_UTILS_MONITOR=deep) also traces co_await'd CoroTasks
// reached by symmetric transfer - synchronous coroutines that never suspend and
// so never reach the executor queue. Inline over a plain bool so the disabled
// co_await path (the common case) is a single load, not a function call.
inline bool monitor_deep_enabled() { return detail::g_monitor_deep; }

// Coroutine execution monitoring, toggled at runtime by env vars (read once and
// cached, so the disabled path is a single branch):
//   DFTRACER_UTILS_MONITOR=1|summary  -> aggregate table at exit
//   DFTRACER_UTILS_MONITOR=tree       -> call tree at exit
//   DFTRACER_UTILS_MONITOR=deep       -> tree incl. synchronous co_await'd
//   coros DFTRACER_UTILS_MONITOR=trace
//     -> per-coroutine live log lines
//     -> per-coroutine CSV
//
// Coverage is universal: every coroutine funnels through Executor::enqueue (the
// spawn path, submitted tasks, and when_all/when_any resumptions all delegate
// there), so hooking enqueue + the worker resume boundary captures the whole
// fan-out without touching any coroutine frame (no GCC frame-layout risk).
//
// The hot path is zero-copy: only the coroutine's resume-function pointer (read
// from the frame, ABI offset 0) plus ids/timestamps are stored. Symbolization
// (dladdr + demangle) happens once per unique function at render time, never on
// the hot path. Per-handle state lives in a sharded map to keep contention low.
bool monitoring_enabled();

// Record the worker-thread id for the calling thread, so spawn/finish threads
// can be reported (and migration between them shown). Called once per worker.
void monitor_set_worker(int worker_id);

// First time `handle` is seen: assign it an id, record its parent (the
// coroutine currently resuming on this thread), its resume-function pointer,
// `kind`, the spawning worker, and start its wall clock. On a re-enqueue the
// existing id is returned. Also called from awaiter suspension points (I/O, and
// in deep mode every co_await'd CoroTask) so coroutines reached by symmetric
// transfer - which never pass through the executor queue until they suspend -
// are captured with their real parent instead of orphaning. Returns the
// coroutine's monitor id (or -1 if monitoring is off).
long long monitor_enqueue(const void* handle, CoroKind kind);

// Mark the start of resuming a coroutine whose monitor id is `id` (carried in
// the run-queue entry, so this is lock-free): it becomes the current coroutine
// so work it enqueues attributes to it.
void monitor_resume_begin(long long id);

// Mark the end of a resume slice. When `done`, finalizes the coroutine's
// wall-clock time and records it.
void monitor_resume_end(const void* handle, bool done);

// Deep mode: finalize a co_await'd child at its await_resume and restore the
// current coroutine to the child's parent. await_resume runs before the
// executor's resume_end (symmetric transfer to the parent happens inside the
// child's resume()), so the child's record still exists here.
void monitor_sync_complete(const void* handle);

}  // namespace dftracer::utils::utilities

#endif  // DFTRACER_UTILS_CORE_UTILITIES_MONITOR_H
