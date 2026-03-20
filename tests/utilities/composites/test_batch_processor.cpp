#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/utilities/behaviors/behavior_chain.h>
#include <dftracer/utils/core/utilities/utility_executor.h>
#include <dftracer/utils/core/utilities/utility_traits.h>
#include <dftracer/utils/utilities/composites/batch_processor_utility.h>
#include <doctest/doctest.h>

#include <numeric>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites;
using namespace dftracer::utils::utilities::behaviors;

namespace tags = dftracer::utils::utilities::tags;

// Test utility with Parallelizable tag for compile-time checks
class StringUppercaseUtility
    : public utilities::Utility<std::string, std::string,
                                utilities::tags::Parallelizable> {
   public:
    coro::CoroTask<std::string> process(const std::string& input) override {
        std::string result = input;
        std::transform(result.begin(), result.end(), result.begin(), ::toupper);
        co_return result;
    }
};

// Test utility that squares integers
class IntSquareUtility
    : public utilities::Utility<int, int, utilities::tags::Parallelizable> {
   public:
    coro::CoroTask<int> process(const int& input) override {
        co_return input* input;
    }
};

// Helper: run a NeedsContext batch utility via Runtime + run_coro_scope.
template <typename ItemInput, typename ItemOutput>
static std::vector<ItemOutput> run_batch(
    std::shared_ptr<BatchProcessorUtility<ItemInput, ItemOutput>> batch,
    const std::vector<ItemInput>& inputs, std::size_t threads = 4) {
    Runtime rt(threads);
    std::vector<ItemOutput> results;
    auto* results_ptr = &results;

    auto task = run_coro_scope(
        rt.executor(),
        [batch, inputs, results_ptr](CoroScope& scope) -> coro::CoroTask<void> {
            UtilityExecutor<std::vector<ItemInput>, std::vector<ItemOutput>,
                            tags::NeedsContext>
                exec(batch, BehaviorChain<std::vector<ItemInput>,
                                          std::vector<ItemOutput>>{});
            *results_ptr = co_await exec.execute_with_context(scope, inputs);
        });

    rt.submit(std::move(task), "batch-run").wait();
    rt.shutdown();
    return results;
}

TEST_SUITE("BatchProcessor") {
    TEST_CASE("BatchProcessor - Basic Processing with Function") {
        SUBCASE("Process strings with lambda") {
            auto processor = [](CoroScope&, const std::string& s) {
                return s + "_processed";
            };

            auto batch = std::make_shared<
                BatchProcessorUtility<std::string, std::string>>(processor);

            std::vector<std::string> inputs = {"hello", "world", "test"};
            auto results = run_batch(batch, inputs);

            CHECK(results.size() == 3);
            CHECK(std::find(results.begin(), results.end(),
                            "hello_processed") != results.end());
            CHECK(std::find(results.begin(), results.end(),
                            "world_processed") != results.end());
            CHECK(std::find(results.begin(), results.end(), "test_processed") !=
                  results.end());
        }

        SUBCASE("Process integers with transformation") {
            auto processor = [](CoroScope&, const int& n) { return n * 2; };

            auto batch =
                std::make_shared<BatchProcessorUtility<int, int>>(processor);

            std::vector<int> inputs = {1, 2, 3, 4, 5};
            auto results = run_batch(batch, inputs);

            CHECK(results.size() == 5);
            CHECK(std::find(results.begin(), results.end(), 2) !=
                  results.end());
            CHECK(std::find(results.begin(), results.end(), 4) !=
                  results.end());
            CHECK(std::find(results.begin(), results.end(), 6) !=
                  results.end());
            CHECK(std::find(results.begin(), results.end(), 8) !=
                  results.end());
            CHECK(std::find(results.begin(), results.end(), 10) !=
                  results.end());
        }

        SUBCASE("Empty input") {
            auto processor = [](CoroScope&, const int& n) { return n * 2; };

            auto batch =
                std::make_shared<BatchProcessorUtility<int, int>>(processor);

            std::vector<int> inputs;
            auto results = run_batch(batch, inputs);
            CHECK(results.empty());
        }
    }

    TEST_CASE("BatchProcessor - With Utility Class") {
        SUBCASE("Process with StringUppercaseUtility") {
            auto utility = std::make_shared<StringUppercaseUtility>();
            auto batch = std::make_shared<
                BatchProcessorUtility<std::string, std::string>>(utility);

            std::vector<std::string> inputs = {"hello", "world", "batch",
                                               "processor"};
            auto results = run_batch(batch, inputs);

            CHECK(results.size() == 4);
            CHECK(std::find(results.begin(), results.end(), "HELLO") !=
                  results.end());
            CHECK(std::find(results.begin(), results.end(), "WORLD") !=
                  results.end());
            CHECK(std::find(results.begin(), results.end(), "BATCH") !=
                  results.end());
            CHECK(std::find(results.begin(), results.end(), "PROCESSOR") !=
                  results.end());
        }

        SUBCASE("Process with IntSquareUtility") {
            auto utility = std::make_shared<IntSquareUtility>();
            auto batch =
                std::make_shared<BatchProcessorUtility<int, int>>(utility);

            std::vector<int> inputs = {1, 2, 3, 4, 5};
            auto results = run_batch(batch, inputs);

            CHECK(results.size() == 5);
            CHECK(std::find(results.begin(), results.end(), 1) !=
                  results.end());
            CHECK(std::find(results.begin(), results.end(), 4) !=
                  results.end());
            CHECK(std::find(results.begin(), results.end(), 9) !=
                  results.end());
            CHECK(std::find(results.begin(), results.end(), 16) !=
                  results.end());
            CHECK(std::find(results.begin(), results.end(), 25) !=
                  results.end());
        }
    }

    TEST_CASE("BatchProcessor - With Comparator") {
        SUBCASE("Sort strings alphabetically") {
            auto processor = [](CoroScope&, const std::string& s) { return s; };

            auto comparator = [](const std::string& a, const std::string& b) {
                return a < b;
            };

            auto batch = std::make_shared<
                BatchProcessorUtility<std::string, std::string>>(processor);
            batch->with_comparator(comparator);

            std::vector<std::string> inputs = {"zebra", "apple", "mango",
                                               "banana"};
            auto results = run_batch(batch, inputs);

            CHECK(results.size() == 4);
            CHECK(results[0] == "apple");
            CHECK(results[1] == "banana");
            CHECK(results[2] == "mango");
            CHECK(results[3] == "zebra");
        }

        SUBCASE("Sort integers descending") {
            auto processor = [](CoroScope&, const int& n) {
                return n * n;  // Square the numbers
            };

            auto comparator = [](const int& a, const int& b) {
                return a > b;  // Descending order
            };

            auto batch =
                std::make_shared<BatchProcessorUtility<int, int>>(processor);
            batch->with_comparator(comparator);

            std::vector<int> inputs = {1, 2, 3, 4, 5};
            auto results = run_batch(batch, inputs);

            CHECK(results.size() == 5);
            CHECK(results[0] == 25);  // 5^2
            CHECK(results[1] == 16);  // 4^2
            CHECK(results[2] == 9);   // 3^2
            CHECK(results[3] == 4);   // 2^2
            CHECK(results[4] == 1);   // 1^2
        }
    }

    TEST_CASE("BatchProcessor - Parallel Execution") {
        SUBCASE("Verify parallel processing results") {
            auto processor = [](CoroScope&, const int& n) { return n * 2; };

            auto batch =
                std::make_shared<BatchProcessorUtility<int, int>>(processor);

            std::vector<int> inputs = {1, 2, 3, 4, 5, 6, 7, 8};
            auto results = run_batch(batch, inputs, 4);

            CHECK(results.size() == 8);
            for (int i = 1; i <= 8; ++i) {
                CHECK(std::find(results.begin(), results.end(), i * 2) !=
                      results.end());
            }
        }

        SUBCASE("Large batch processing") {
            auto processor = [](CoroScope&, const int& n) { return n * n; };

            auto batch =
                std::make_shared<BatchProcessorUtility<int, int>>(processor);

            std::vector<int> inputs(1000);
            std::iota(inputs.begin(), inputs.end(), 1);  // Fill with 1..1000

            auto results = run_batch(batch, inputs, 8);

            CHECK(results.size() == 1000);

            CHECK(std::find(results.begin(), results.end(), 1) !=
                  results.end());  // 1^2
            CHECK(std::find(results.begin(), results.end(), 4) !=
                  results.end());  // 2^2
            CHECK(std::find(results.begin(), results.end(), 9) !=
                  results.end());  // 3^2
            CHECK(std::find(results.begin(), results.end(), 1000000) !=
                  results.end());  // 1000^2
        }
    }
}
