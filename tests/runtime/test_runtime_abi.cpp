#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/runtime/coro/abi.h>
#include <dftracer/utils/runtime/coro/task_abi.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <atomic>

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
    TEST_CASE("every dftu_runtime_new returns the process runtime") {
        dftu_runtime* rt1 = dftu_runtime_new(2, 2);
        dftu_runtime* rt2 = dftu_runtime_new(3, 2);
        CHECK(rt1 == rt2);
        CHECK(dftu_default_runtime() == rt1);
        CHECK(reinterpret_cast<Runtime*>(rt1)->threads() == 3);
        dftu_runtime_free(rt2);
        dftu_runtime_free(rt1);
    }

    TEST_CASE("dftu_runtime_new moves the thread cap both ways") {
        auto* runtime = reinterpret_cast<Runtime*>(dftu_runtime_new(4, 0));
        CHECK(runtime->threads() == 4);
        CHECK(reinterpret_cast<Runtime*>(dftu_runtime_new(2, 0))->threads() ==
              2);
        CHECK(exercise(dftu_default_runtime()));
    }

    TEST_CASE("threads=0 keeps the current count") {
        dftu_runtime_new(3, 0);
        auto* runtime = reinterpret_cast<Runtime*>(dftu_runtime_new(0, 0));
        CHECK(runtime->threads() == 3);
    }

    TEST_CASE("dftu_runtime_free leaves the runtime running") {
        dftu_runtime* rt = dftu_runtime_new(2, 1);
        REQUIRE(rt != nullptr);
        dftu_runtime_free(rt);
        dftu_runtime_free(nullptr);
        CHECK(exercise(dftu_default_runtime()));
        CHECK(exercise(rt));
    }
}
