#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/abi.h>
#include <dftracer/utils/core/coro/task_abi.h>
#include <doctest/doctest.h>

#include <atomic>

using dftracer::utils::task_to_abi;
using dftracer::utils::coro::CoroTask;

namespace {

CoroTask<void> set_flag(std::atomic<int>* n) {
    n->fetch_add(1);
    co_return;
}

}  // namespace

TEST_SUITE("core task ABI") {
    TEST_CASE("dftu_task_run drives a task to completion") {
        std::atomic<int> ran{0};
        dftu_task* t = task_to_abi(set_flag(&ran));
        CHECK(dftu_task_run(dftu_default_runtime(), t) == 0);
        CHECK(ran.load() == 1);
    }

    TEST_CASE("dftu_task_run reports a thrown task") {
        dftu_task* t = task_to_abi([]() -> CoroTask<void> {
            throw std::runtime_error("boom");
            co_return;
        }());
        CHECK(dftu_task_run(dftu_default_runtime(), t) == -1);
    }

    TEST_CASE("dftu_task_when_all waits for every input") {
        std::atomic<int> ran{0};
        dftu_task* ts[3] = {task_to_abi(set_flag(&ran)),
                            task_to_abi(set_flag(&ran)),
                            task_to_abi(set_flag(&ran))};
        dftu_task* all = dftu_task_when_all(ts, 3);
        CHECK(dftu_task_run(dftu_default_runtime(), all) == 0);
        CHECK(ran.load() == 3);
    }

    TEST_CASE("dftu_task_when_any completes on the first") {
        std::atomic<int> ran{0};
        dftu_task* ts[2] = {task_to_abi(set_flag(&ran)),
                            task_to_abi(set_flag(&ran))};
        dftu_task* any = dftu_task_when_any(ts, 2);
        CHECK(dftu_task_run(dftu_default_runtime(), any) == 0);
        CHECK(ran.load() >= 1);
    }

    TEST_CASE("empty when_all/when_any return null") {
        CHECK(dftu_task_when_all(nullptr, 0) == nullptr);
        CHECK(dftu_task_when_any(nullptr, 0) == nullptr);
    }
}
