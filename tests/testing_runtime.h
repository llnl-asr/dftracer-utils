// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_TESTING_RUNTIME_H
#define DFTRACER_UTILS_TESTS_TESTING_RUNTIME_H

// Runtime plumbing shared by the tests that drive coroutines. Kept out of
// testing_utilities.h so the 160-odd tests that never touch the runtime do not
// pay for its headers.

#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/run_loop.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/runtime/runtime.h>

#include <cstddef>
#include <utility>

namespace dftu_utils_test {

/// The width a pool of @p n threads gets. Valgrind mode caps every compute and
/// I/O pool at two threads, so a test that checks a width asks for this one.
constexpr std::size_t pool_width(std::size_t n) {
#ifdef DFTRACER_UTILS_VALGRIND_MODE
    return n < 2 ? n : 2;
#else
    return n;
#endif
}

/// Run `fn` as a coroutine on a private Runtime and block until it finishes.
/// The runtime is shut down before returning, so anything the body spawned has
/// completed by then. `tag` names the submission in the runtime's logs.
template <typename Fn>
void run_coro(Fn&& fn, const char* tag = "test") {
    dftracer::utils::Runtime rt(4);
    rt.submit(dftracer::utils::coro::with_scope(std::forward<Fn>(fn)), tag)
        .wait();
    rt.shutdown();
}

/// Drives the task on the calling thread, as get() did before it used the
/// process pool.
template <class T>
T run_here(dftracer::utils::coro::CoroTask<T> task) {
    dftracer::utils::coro::RunLoop loop;
    dftracer::utils::coro::CurrentScope bind{loop};
    return task.get();
}

}  // namespace dftu_utils_test

#endif  // DFTRACER_UTILS_TESTS_TESTING_RUNTIME_H
