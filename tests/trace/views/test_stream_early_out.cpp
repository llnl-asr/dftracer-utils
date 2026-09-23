#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/runtime.h>
#include <doctest/doctest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "test_view_common.h"

namespace {

// A sentinel captured by the source View's cancel predicate. The predicate is
// copied into the plan, the plan into the View the scan coroutine owns, so the
// last strong reference dies with that coroutine's frame: the weak_ptr expiring
// means the producer actually finished.
struct ProducerSentinel {
    std::shared_ptr<int> strong = std::make_shared<int>(0);
    std::weak_ptr<int> weak = strong;
};

bool wait_until_expired(const std::weak_ptr<int>& w) {
    for (int i = 0; i < 400 && !w.expired(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return w.expired();
}

}  // namespace

TEST_SUITE("View - streaming early-out") {
    TEST_CASE("abandoning a stream stops the scan behind it") {
        TestEnvironment env(200);
        // Enough members that a whole-trace scan needs many units, and a byte
        // budget small enough that the producer must wait on the consumer.
        std::string gz =
            test_view_common::create_multimember_trace(env, 4000, 512);
        std::string idx = determine_index_path(gz, "");

        ProducerSentinel sentinel;
        {
            View v =
                View::from_file(gz, idx)
                    .metadata(false)
                    .memory_budget(4096)
                    .cancel_when([keep = sentinel.strong] { return false; });

            auto pulled =
                dftracer::utils::default_runtime()
                    .submit([](View vv) -> coro::CoroTask<std::int64_t> {
                        auto gen = vv.stream(64);
                        auto first = co_await gen.next();
                        co_return first ? first->num_rows() : 0;
                    }(v))
                    .get();
            CHECK(pulled > 0);
        }
        sentinel.strong.reset();

        // Before the fix the producer kept scanning into a channel nobody
        // drained and then parked forever in the budget semaphore, so this
        // reference never dropped.
        CHECK(wait_until_expired(sentinel.weak));
    }

    TEST_CASE("a fully drained stream still returns every row") {
        TestEnvironment env(200);
        std::string gz =
            test_view_common::create_multimember_trace(env, 500, 512);
        std::string idx = determine_index_path(gz, "");
        View v = View::from_file(gz, idx).metadata(false).memory_budget(4096);

        const std::int64_t rows =
            dftracer::utils::default_runtime()
                .submit([](View vv) -> coro::CoroTask<std::int64_t> {
                    std::int64_t n = 0;
                    auto gen = vv.stream(64);
                    while (auto chunk = co_await gen.next())
                        n += chunk->num_rows();
                    co_return n;
                }(v))
                .get();
        CHECK(rows == 500);
    }
}
