#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/coro/abi.h>
#include <dftracer/utils/core/coro/task_abi.h>
#include <dftracer/utils/core/runtime.h>
#include <doctest/doctest.h>

#include <atomic>
#include <memory>

using dftracer::utils::hardware_concurrency;
using dftracer::utils::Runtime;
using dftracer::utils::task_to_abi;
using dftracer::utils::coro::CoroTask;

namespace {

CoroTask<void> set_flag(std::atomic<int>* n) {
    n->fetch_add(1);
    co_return;
}

// Run a no-op task through rt so a use-after-free on rt is a real memory
// access (ASan catches it), not just a pointer comparison.
bool exercise(dftu_runtime* rt) {
    std::atomic<int> ran{0};
    dftu_task* t = task_to_abi(set_flag(&ran));
    return dftu_task_run(rt, t) == 0 && ran.load() == 1;
}

}  // namespace

TEST_SUITE("core runtime ABI") {
    TEST_CASE("dftu_runtime_new installs the default when none is set") {
        dftu_set_default_runtime(nullptr);

        dftu_runtime* rt1 = dftu_runtime_new(2, 2);
        CHECK(dftu_default_runtime() == rt1);

        dftu_runtime_free(rt1);
    }

    TEST_CASE("a second dftu_runtime_new does not steal the default") {
        dftu_set_default_runtime(nullptr);

        dftu_runtime* rt1 = dftu_runtime_new(2, 2);
        CHECK(dftu_default_runtime() == rt1);

        dftu_runtime* rt2 = dftu_runtime_new(2, 2);
        CHECK(dftu_default_runtime() == rt1);

        dftu_runtime_free(rt2);
        dftu_runtime_free(rt1);
    }

    TEST_CASE("dftu_set_default_runtime overrides the current default") {
        dftu_set_default_runtime(nullptr);

        dftu_runtime* rt1 = dftu_runtime_new(2, 2);
        dftu_runtime* rt2 = dftu_runtime_new(2, 2);
        CHECK(dftu_default_runtime() == rt1);

        dftu_set_default_runtime(rt2);
        CHECK(dftu_default_runtime() == rt2);

        dftu_runtime_free(rt2);
        dftu_runtime_free(rt1);
    }

    TEST_CASE(
        "freeing the current default reverts to a usable lazy fallback, not "
        "a dangling pointer") {
        dftu_set_default_runtime(nullptr);

        dftu_runtime* rt1 = dftu_runtime_new(2, 2);
        CHECK(dftu_default_runtime() == rt1);

        dftu_runtime_free(rt1);

        // A NULL-means-default lookup after the free must land on a fresh
        // lazy runtime, not the freed rt1. Actually running a task through it
        // forces a real memory access to the runtime, so a missing
        // clear-on-free (a dangling default) is caught by ASan, not merely a
        // pointer-equality check that could pass by allocator coincidence.
        dftu_runtime* fallback = dftu_default_runtime();
        CHECK(exercise(fallback));

        dftu_set_default_runtime(nullptr);
    }

    TEST_CASE(
        "dftu_runtime_new replaces a lazy fallback but not a user default") {
        dftu_set_default_runtime(nullptr);

        // Trigger creation of the lazy fallback.
        dftu_runtime* lazy = dftu_default_runtime();
        CHECK(exercise(lazy));

        dftu_runtime* rt1 = dftu_runtime_new(2, 2);
        CHECK(dftu_default_runtime() == rt1);

        dftu_runtime* rt2 = dftu_runtime_new(2, 2);
        CHECK(dftu_default_runtime() == rt1);

        dftu_runtime_free(rt2);
        dftu_runtime_free(rt1);
        dftu_set_default_runtime(nullptr);
    }

    TEST_CASE("threads=0 gives a parallel runtime, not a serial one") {
        dftu_runtime* rt = dftu_runtime_new(0, 0);
        auto* runtime = reinterpret_cast<Runtime*>(rt);
        CHECK(runtime->threads() == hardware_concurrency());
        CHECK(runtime->threads() > 0);
        dftu_runtime_free(rt);
    }
}

// default_runtime_shared() hands out copies, and the Python Runtime object
// stores one that outlives the call. Freeing the ABI handle must therefore drop
// a reference, not delete: a non-owning shared_ptr would leave every other
// holder pointing at freed memory with a refcount still claiming it was alive.
TEST_CASE("a retained reference outlives dftu_runtime_free") {
    dftu_runtime* rt = dftu_runtime_new(2, 1);
    REQUIRE(rt != nullptr);

    std::shared_ptr<dftracer::utils::Runtime> held =
        dftracer::utils::default_runtime_shared();
    REQUIRE(held.get() == reinterpret_cast<dftracer::utils::Runtime*>(rt));

    dftu_runtime_free(rt);

    // The ABI's reference is gone, but `held` still owns the runtime, so
    // touching it here must be a live read rather than a use-after-free.
    CHECK(held.use_count() >= 1);
    CHECK(held->executor() != nullptr);
}
