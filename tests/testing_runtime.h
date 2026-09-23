#ifndef DFTRACER_UTILS_TESTS_TESTING_RUNTIME_H
#define DFTRACER_UTILS_TESTS_TESTING_RUNTIME_H

// Runtime plumbing shared by the tests that drive coroutines. Kept out of
// testing_utilities.h so the 160-odd tests that never touch the runtime do not
// pay for its headers.

#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>

#include <utility>

namespace dftu_utils_test {

/// Run `fn` as a coroutine on a private Runtime and block until it finishes.
/// The runtime is shut down before returning, so anything the body spawned has
/// completed by then. `tag` names the submission in the runtime's logs.
template <typename Fn>
void run_coro(Fn&& fn, const char* tag = "test") {
    dftracer::utils::Runtime rt(4);
    auto task =
        dftracer::utils::run_coro_scope(rt.executor(), std::forward<Fn>(fn));
    rt.submit(std::move(task), tag).wait();
    rt.shutdown();
}

}  // namespace dftu_utils_test

#endif  // DFTRACER_UTILS_TESTS_TESTING_RUNTIME_H
