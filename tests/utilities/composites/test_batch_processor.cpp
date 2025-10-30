#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/pipeline/executor.h>
#include <dftracer/utils/core/pipeline/scheduler.h>
#include <dftracer/utils/core/tasks/task_context.h>
#include <dftracer/utils/core/utilities/utility_adapter.h>
#include <dftracer/utils/core/utilities/utility_traits.h>
#include <dftracer/utils/utilities/composites/batch_processor_utility.h>
#include <doctest/doctest.h>

#include <chrono>
#include <numeric>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;
using namespace dftracer::utils::utilities::composites;

// Test utility with Parallelizable tag for compile-time checks
class StringUppercaseUtility
    : public utilities::Utility<std::string, std::string,
                                utilities::tags::Parallelizable> {
   public:
    std::string process(const std::string& input) override {
        std::string result = input;
        std::transform(result.begin(), result.end(), result.begin(), ::toupper);
        return result;
    }
};

// Test utility that squares integers
class IntSquareUtility
    : public utilities::Utility<int, int, utilities::tags::Parallelizable> {
   public:
    int process(const int& input) override { return input * input; }
};

TEST_SUITE("BatchProcessor") {
    TEST_CASE("BatchProcessor - Basic Processing with Function") {
        SUBCASE("Process strings with lambda") {
            // Create batch processor with a lambda
            auto processor = [](TaskContext&, const std::string& s) {
                return s + "_processed";
            };

            auto batch = std::make_shared<
                BatchProcessorUtility<std::string, std::string>>(processor);

            // Set up executor and scheduler
            Executor executor(4);
            Scheduler scheduler(&executor);

            // Use the adapter to convert utility to task
            auto batch_task = use(batch).as_task();

            // Schedule the task
            std::vector<std::string> inputs = {"hello", "world", "test"};
            scheduler.schedule(batch_task, inputs);

            // Wait for completion
            // Get results
            auto results = batch_task->get<std::vector<std::string>>();

            CHECK(results.size() == 3);
            CHECK(std::find(results.begin(), results.end(),
                            "hello_processed") != results.end());
            CHECK(std::find(results.begin(), results.end(),
                            "world_processed") != results.end());
            CHECK(std::find(results.begin(), results.end(), "test_processed") !=
                  results.end());

            // Explicit shutdown to prevent race condition
            executor.shutdown();
        }

        SUBCASE("Process integers with transformation") {
            auto processor = [](TaskContext&, const int& n) { return n * 2; };

            auto batch =
                std::make_shared<BatchProcessorUtility<int, int>>(processor);

            Executor executor(4);
            Scheduler scheduler(&executor);

            auto batch_task = use(batch).as_task();

            std::vector<int> inputs = {1, 2, 3, 4, 5};
            scheduler.schedule(batch_task, inputs);

            auto results = batch_task->get<std::vector<int>>();

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

            // Explicit shutdown to prevent race condition
            executor.shutdown();
        }

        SUBCASE("Empty input") {
            auto processor = [](TaskContext&, const int& n) { return n * 2; };

            auto batch =
                std::make_shared<BatchProcessorUtility<int, int>>(processor);

            Executor executor(4);
            Scheduler scheduler(&executor);

            auto batch_task = use(batch).as_task();

            std::vector<int> inputs;
            scheduler.schedule(batch_task, inputs);

            auto results = batch_task->get<std::vector<int>>();
            CHECK(results.empty());

            // Explicit shutdown to prevent race condition
            executor.shutdown();
        }
    }

    TEST_CASE("BatchProcessor - With Utility Class") {
        SUBCASE("Process with StringUppercaseUtility") {
            auto utility = std::make_shared<StringUppercaseUtility>();
            auto batch = std::make_shared<
                BatchProcessorUtility<std::string, std::string>>(utility);

            Executor executor(4);
            Scheduler scheduler(&executor);

            auto batch_task = use(batch).as_task();

            std::vector<std::string> inputs = {"hello", "world", "batch",
                                               "processor"};
            scheduler.schedule(batch_task, inputs);

            auto results = batch_task->get<std::vector<std::string>>();

            CHECK(results.size() == 4);
            CHECK(std::find(results.begin(), results.end(), "HELLO") !=
                  results.end());
            CHECK(std::find(results.begin(), results.end(), "WORLD") !=
                  results.end());
            CHECK(std::find(results.begin(), results.end(), "BATCH") !=
                  results.end());
            CHECK(std::find(results.begin(), results.end(), "PROCESSOR") !=
                  results.end());

            // Explicit shutdown to prevent race condition
            executor.shutdown();
        }

        SUBCASE("Process with IntSquareUtility") {
            auto utility = std::make_shared<IntSquareUtility>();
            auto batch =
                std::make_shared<BatchProcessorUtility<int, int>>(utility);

            Executor executor(4);
            Scheduler scheduler(&executor);

            auto batch_task = use(batch).as_task();

            std::vector<int> inputs = {1, 2, 3, 4, 5};
            scheduler.schedule(batch_task, inputs);

            auto results = batch_task->get<std::vector<int>>();

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

            // Explicit shutdown to prevent race condition
            executor.shutdown();
        }
    }

    TEST_CASE("BatchProcessor - With Comparator") {
        SUBCASE("Sort strings alphabetically") {
            auto processor = [](TaskContext&, const std::string& s) {
                return s;
            };

            auto comparator = [](const std::string& a, const std::string& b) {
                return a < b;
            };

            auto batch = std::make_shared<
                BatchProcessorUtility<std::string, std::string>>(processor);
            batch->with_comparator(comparator);

            Executor executor(4);
            Scheduler scheduler(&executor);

            auto batch_task = use(batch).as_task();

            std::vector<std::string> inputs = {"zebra", "apple", "mango",
                                               "banana"};
            scheduler.schedule(batch_task, inputs);

            auto results = batch_task->get<std::vector<std::string>>();

            CHECK(results.size() == 4);
            CHECK(results[0] == "apple");
            CHECK(results[1] == "banana");
            CHECK(results[2] == "mango");
            CHECK(results[3] == "zebra");

            // Explicit shutdown to prevent race condition
            executor.shutdown();
        }

        SUBCASE("Sort integers descending") {
            auto processor = [](TaskContext&, const int& n) {
                return n * n;  // Square the numbers
            };

            auto comparator = [](const int& a, const int& b) {
                return a > b;  // Descending order
            };

            auto batch =
                std::make_shared<BatchProcessorUtility<int, int>>(processor);
            batch->with_comparator(comparator);

            Executor executor(4);
            Scheduler scheduler(&executor);

            auto batch_task = use(batch).as_task();

            std::vector<int> inputs = {1, 2, 3, 4, 5};
            scheduler.schedule(batch_task, inputs);

            auto results = batch_task->get<std::vector<int>>();

            CHECK(results.size() == 5);
            CHECK(results[0] == 25);  // 5^2
            CHECK(results[1] == 16);  // 4^2
            CHECK(results[2] == 9);   // 3^2
            CHECK(results[3] == 4);   // 2^2
            CHECK(results[4] == 1);   // 1^2

            // Explicit shutdown to prevent race condition
            executor.shutdown();
        }
    }

    TEST_CASE("BatchProcessor - Parallel Execution") {
        SUBCASE("Verify parallel processing results") {
            // Processor that doubles the value
            auto processor = [](TaskContext&, const int& n) {
                // Simulate some work
                return n * 2;
            };

            auto batch =
                std::make_shared<BatchProcessorUtility<int, int>>(processor);

            // Use 4 threads
            Executor executor(4);
            Scheduler scheduler(&executor);

            auto batch_task = use(batch).as_task();

            // Process 8 items in parallel
            std::vector<int> inputs = {1, 2, 3, 4, 5, 6, 7, 8};
            scheduler.schedule(batch_task, inputs);

            // Wait for completion
            auto results = batch_task->get<std::vector<int>>();

            // Verify all items were processed correctly
            CHECK(results.size() == 8);
            for (int i = 1; i <= 8; ++i) {
                CHECK(std::find(results.begin(), results.end(), i * 2) !=
                      results.end());
            }

            // Explicit shutdown to prevent race condition
            executor.shutdown();
        }

        SUBCASE("Large batch processing") {
            // Process a large number of items
            auto processor = [](TaskContext&, const int& n) { return n * n; };

            auto batch =
                std::make_shared<BatchProcessorUtility<int, int>>(processor);

            Executor executor(8);  // Use more threads
            Scheduler scheduler(&executor);

            auto batch_task = use(batch).as_task();

            // Create large input
            std::vector<int> inputs(1000);
            std::iota(inputs.begin(), inputs.end(), 1);  // Fill with 1..1000

            scheduler.schedule(batch_task, inputs);

            auto results = batch_task->get<std::vector<int>>();

            CHECK(results.size() == 1000);

            // Verify some results
            CHECK(std::find(results.begin(), results.end(), 1) !=
                  results.end());  // 1^2
            CHECK(std::find(results.begin(), results.end(), 4) !=
                  results.end());  // 2^2
            CHECK(std::find(results.begin(), results.end(), 9) !=
                  results.end());  // 3^2
            CHECK(std::find(results.begin(), results.end(), 1000000) !=
                  results.end());  // 1000^2

            // Explicit shutdown to prevent race condition
            executor.shutdown();
        }
    }
}
