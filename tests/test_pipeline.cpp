#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/pipeline/error.h>
#include <dftracer/utils/core/pipeline/executors/sequential_executor.h>
#include <dftracer/utils/core/pipeline/executors/thread_executor.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/function_task.h>
#include <doctest/doctest.h>

#include <any>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace dftracer::utils;

TEST_CASE("Pipeline - Basic functionality") {
    Pipeline pipeline;

    auto double_task = [](int input, TaskContext&) -> int { return input * 2; };

    auto task_result = pipeline.add_task<int, int>(double_task);
    CHECK(task_result.id() == 0);
}

TEST_CASE("Pipeline - Sequential execution") {
    Pipeline pipeline;

    auto double_task = [](int input, TaskContext&) -> int { return input * 2; };

    auto task_result = pipeline.add_task<int, int>(double_task);

    SequentialExecutor executor;
    executor.execute(pipeline, 21);
    int final_result = task_result.get();

    CHECK(final_result == 42);
}

TEST_CASE("Pipeline - Thread execution") {
    Pipeline pipeline;

    auto double_task = [](int input, TaskContext&) -> int { return input * 2; };

    auto task_result = pipeline.add_task<int, int>(double_task);

    ThreadExecutor executor(2);
    executor.execute(pipeline, 21);
    int final_result = task_result.get();

    CHECK(final_result == 42);
}

TEST_CASE("Pipeline - Task dependencies") {
    Pipeline pipeline;

    auto add_task = [](int input, TaskContext&) -> int { return input + 10; };

    auto multiply_task = [](int input, TaskContext&) -> int {
        return input * 2;
    };

    auto t1 = pipeline.add_task<int, int>(add_task);
    auto t2 = pipeline.add_task<int, int>(multiply_task);
    pipeline.add_dependency(t1.id(), t2.id());

    SequentialExecutor executor;
    executor.execute(pipeline, 5);

    CHECK(t1.get() == 15);
    CHECK(t2.get() == 30);
}

TEST_CASE("Pipeline - Task emission") {
    Pipeline pipeline;

    auto emitting_task = [](int input, TaskContext& ctx) -> int {
        auto child_task = [](int x, TaskContext&) -> int { return x * 3; };

        ctx.emit<int, int>(child_task, Input{input * 2},
                           DependsOn{ctx.current()});
        return input + 5;
    };

    pipeline.add_task<int, int>(emitting_task);

    SequentialExecutor executor;
    PipelineOutput result = executor.execute(pipeline, 10);
    int final_result = result.get<int>();

    CHECK(final_result == 15);
}

TEST_CASE("Pipeline - String processing") {
    Pipeline pipeline;

    auto string_task = [](std::string input, TaskContext&) -> std::string {
        return "Processed: " + input;
    };

    pipeline.add_task<std::string, std::string>(string_task);

    SequentialExecutor executor;
    std::string input = "test";
    PipelineOutput result = executor.execute(pipeline, input);
    std::string final_result = result.get<std::string>();

    CHECK(final_result == "Processed: test");
}

TEST_CASE("Pipeline - Vector processing") {
    Pipeline pipeline;

    auto vector_task = [](std::vector<int> input, TaskContext&) -> int {
        int sum = 0;
        for (const auto& elem : input) {
            sum += elem;
        }
        return sum;
    };

    pipeline.add_task<std::vector<int>, int>(vector_task);

    SequentialExecutor executor;
    std::vector<int> input = {1, 2, 3, 4, 5};
    PipelineOutput result = executor.execute(pipeline, input);
    int final_result = result.get<int>();

    CHECK(final_result == 15);
}

TEST_CASE("Pipeline - Deterministic execution") {
    Pipeline pipeline;

    std::atomic<int> counter{0};

    auto deterministic_task = [&counter](int input, TaskContext& ctx) -> int {
        for (int i = 0; i < 5; ++i) {
            auto work_task = [&counter, i, input](int work_amount,
                                                  TaskContext&) -> int {
                counter++;
                int result = input;
                for (int j = 0; j < work_amount * 10; ++j) {
                    result = (result * 3 + 7) % 1000;
                }
                return result + i;
            };

            ctx.emit<int, int>(work_task, Input{i + 1});
        }
        return input * 2;
    };

    pipeline.add_task<int, int>(deterministic_task);

    SequentialExecutor seq_executor;
    counter = 0;
    PipelineOutput seq_result = seq_executor.execute(pipeline, 42);
    int seq_final = std::any_cast<int>(seq_result.begin()->second);
    int seq_counter = counter.load();

    ThreadExecutor thread_executor(2);
    counter = 0;
    PipelineOutput thread_result = thread_executor.execute(pipeline, 42);
    int thread_final = std::any_cast<int>(thread_result.begin()->second);
    int thread_counter = counter.load();

    CHECK(seq_final == thread_final);
    CHECK(seq_counter == thread_counter);
}

TEST_CASE("Pipeline - Multiple task chains") {
    Pipeline pipeline;

    auto task1 = [](int input, TaskContext&) -> int { return input + 1; };

    auto task2 = [](int input, TaskContext&) -> int { return input * 2; };

    auto task3 = [](int input, TaskContext&) -> int { return input - 5; };

    auto t1 = pipeline.add_task<int, int>(task1);
    auto t2 = pipeline.add_task<int, int>(task2);
    auto t3 = pipeline.add_task<int, int>(task3);

    pipeline.add_dependency(t1.id(), t2.id());
    pipeline.add_dependency(t2.id(), t3.id());

    SequentialExecutor executor;
    PipelineOutput result = executor.execute(pipeline, 10);
    int final_result = result.get<int>();

    CHECK(final_result == 17);
}

TEST_CASE("Pipeline - Complex task emission") {
    Pipeline pipeline;

    auto generator = [](std::vector<int> input, TaskContext& ctx) -> int {
        int sum = 0;
        for (size_t i = 0; i < input.size(); ++i) {
            auto element_processor = [i](int element, TaskContext&) -> int {
                return element * element;
            };

            ctx.emit<int, int>(element_processor, Input{input[i]},
                               DependsOn{ctx.current()});
            sum += input[i];
        }
        return sum;
    };

    pipeline.add_task<std::vector<int>, int>(generator);

    SequentialExecutor executor;
    std::vector<int> input = {2, 3, 4};
    PipelineOutput result = executor.execute(pipeline, input);
    int final_result = result.get<int>();

    CHECK(final_result == 9);
}

TEST_CASE("Pipeline - Thread safety") {
    Pipeline pipeline;

    std::atomic<int> shared_counter{0};

    auto thread_safe_task = [&shared_counter](int input,
                                              TaskContext& ctx) -> int {
        for (int i = 0; i < 10; ++i) {
            auto atomic_task = [&shared_counter, i](int x,
                                                    TaskContext&) -> int {
                shared_counter++;
                return x + i;
            };

            ctx.emit<int, int>(atomic_task, Input{input});
        }
        return input;
    };

    pipeline.add_task<int, int>(thread_safe_task);

    ThreadExecutor executor(4);
    PipelineOutput result = executor.execute(pipeline, 5);
    int final_result = result.get<int>();

    CHECK(final_result == 5);
    CHECK(shared_counter.load() == 10);
}

TEST_CASE("Pipeline - Error handling") {
    Pipeline pipeline;

    auto error_task = [](int input, TaskContext&) -> int {
        if (input < 0) {
            return -1;
        }
        return input * 2;
    };

    pipeline.add_task<int, int>(error_task);

    SequentialExecutor executor;

    SUBCASE("Valid input") {
        PipelineOutput result = executor.execute(pipeline, 5);
        int final_result = result.get<int>();
        CHECK(final_result == 10);
    }

    SUBCASE("Invalid input") {
        PipelineOutput result = executor.execute(pipeline, -5);
        int final_result = result.get<int>();
        CHECK(final_result == -1);
    }
}

TEST_CASE("Pipeline - Empty pipeline") {
    Pipeline pipeline;
    SequentialExecutor executor;

    CHECK_THROWS(executor.execute(pipeline, 42));
}

TEST_CASE("Pipeline - Different executor thread counts") {
    Pipeline pipeline;

    auto simple_task = [](int input, TaskContext&) -> int { return input * 3; };

    pipeline.add_task<int, int>(simple_task);

    std::vector<int> thread_counts = {1, 2, 4, 8};

    for (int thread_count : thread_counts) {
        ThreadExecutor executor(thread_count);
        PipelineOutput result = executor.execute(pipeline, 7);
        int final_result = result.get<int>();
        CHECK(final_result == 21);
    }
}

TEST_CASE("Pipeline - Cyclic dependency detection") {
    Pipeline pipeline;

    auto task1 = [](int input, TaskContext&) -> int { return input + 1; };

    auto task2 = [](int input, TaskContext&) -> int { return input * 2; };

    auto t1 = pipeline.add_task<int, int>(task1);
    auto t2 = pipeline.add_task<int, int>(task2);

    pipeline.add_dependency(t1.id(), t2.id());
    pipeline.add_dependency(t2.id(), t1.id());

    SequentialExecutor executor;
    CHECK_THROWS_AS(executor.execute(pipeline, 5), PipelineError);
}

TEST_CASE("Pipeline - Type mismatch validation") {
    Pipeline pipeline;

    auto string_task = [](int input, TaskContext&) -> std::string {
        return std::to_string(input);
    };

    auto int_task = [](int input, TaskContext&) -> int { return input * 2; };

    auto t1 = pipeline.add_task<int, std::string>(string_task);
    auto t2 = pipeline.add_task<int, int>(int_task);

    pipeline.add_dependency(t1.id(), t2.id());

    SequentialExecutor executor;
    CHECK_THROWS_AS(executor.execute(pipeline, 5), PipelineError);
}

TEST_CASE("Pipeline - Multiple dependencies") {
    Pipeline pipeline;

    auto task1 = [](int input, TaskContext&) -> int { return input + 10; };

    auto task2 = [](int input, TaskContext&) -> int { return input * 2; };

    auto combiner_task = [](std::vector<std::any> inputs, TaskContext&) -> int {
        int sum = 0;
        for (const auto& input : inputs) {
            sum += std::any_cast<int>(input);
        }
        return sum;
    };

    auto t1 = pipeline.add_task<int, int>(task1);
    auto t2 = pipeline.add_task<int, int>(task2);
    auto t3 = pipeline.add_task<std::vector<std::any>, int>(combiner_task);

    pipeline.add_dependency(t1.id(), t3.id());
    pipeline.add_dependency(t2.id(), t3.id());

    SequentialExecutor executor;
    PipelineOutput result = executor.execute(pipeline, 5);
    int final_result = result.get<int>();

    CHECK(final_result == 25);
}

TEST_CASE("Pipeline - Multiple dependencies type mismatch") {
    Pipeline pipeline;

    auto task1 = [](int input, TaskContext&) -> int { return input + 10; };

    auto task2 = [](int input, TaskContext&) -> int { return input * 2; };

    auto bad_combiner = [](int input, TaskContext&) -> int { return input; };

    auto t1 = pipeline.add_task<int, int>(task1);
    auto t2 = pipeline.add_task<int, int>(task2);
    auto t3 = pipeline.add_task<int, int>(bad_combiner);

    pipeline.add_dependency(t1.id(), t3.id());
    pipeline.add_dependency(t2.id(), t3.id());

    SequentialExecutor executor;
    CHECK_THROWS_AS(executor.execute(pipeline, 5), PipelineError);
}

TEST_CASE("Pipeline - Complex dependency graph") {
    Pipeline pipeline;

    auto add_task = [](int input, TaskContext&) -> int { return input + 1; };

    auto multiply_task = [](int input, TaskContext&) -> int {
        return input * 2;
    };

    auto combiner_task = [](std::vector<std::any> inputs, TaskContext&) -> int {
        int product = 1;
        for (const auto& input : inputs) {
            product *= std::any_cast<int>(input);
        }
        return product;
    };

    auto t1 = pipeline.add_task<int, int>(add_task);
    auto t2 = pipeline.add_task<int, int>(multiply_task);
    auto t3 = pipeline.add_task<int, int>(add_task);
    auto t4 = pipeline.add_task<int, int>(multiply_task);
    auto t5 = pipeline.add_task<std::vector<std::any>, int>(combiner_task);

    pipeline.add_dependency(t1.id(), t2.id());
    pipeline.add_dependency(t1.id(), t3.id());
    pipeline.add_dependency(t2.id(), t5.id());
    pipeline.add_dependency(t3.id(), t4.id());
    pipeline.add_dependency(t4.id(), t5.id());

    SequentialExecutor executor;
    PipelineOutput result = executor.execute(pipeline, 2);
    int final_result = result.get<int>();

    CHECK(final_result == 48);
}

TEST_CASE("Pipeline - Task context usage") {
    Pipeline pipeline;

    std::vector<TaskIndex> emitted_tasks;

    auto context_task = [&emitted_tasks](int input, TaskContext& ctx) -> int {
        auto child_task = [input](int multiplier, TaskContext&) -> int {
            return input * multiplier;
        };

        auto child_result = ctx.emit<int, int>(child_task, Input{3});
        emitted_tasks.push_back(child_result.id());

        return input + 5;
    };

    pipeline.add_task<int, int>(context_task);

    SequentialExecutor executor;
    PipelineOutput result = executor.execute(pipeline, 10);
    int final_result = result.get<int>();

    CHECK(final_result == 15);
    CHECK(emitted_tasks.size() == 1);
}

TEST_CASE("Pipeline - Empty pipeline validation") {
    Pipeline empty_pipeline;

    SequentialExecutor seq_executor;
    ThreadExecutor thread_executor(2);

    CHECK_THROWS_AS(seq_executor.execute(empty_pipeline, 42), PipelineError);
    CHECK_THROWS_AS(thread_executor.execute(empty_pipeline, 42), PipelineError);
}

TEST_CASE("Pipeline - Large pipeline stress test") {
    Pipeline pipeline;

    std::vector<TaskIndex> tasks;

    for (int i = 0; i < 100; ++i) {
        auto task = [i](int input, TaskContext&) -> int { return input + i; };

        auto task_result = pipeline.add_task<int, int>(task);
        tasks.push_back(task_result.id());

        if (i > 0) {
            pipeline.add_dependency(tasks[i - 1], tasks[i]);
        }
    }

    SequentialExecutor executor;
    PipelineOutput result = executor.execute(pipeline, 0);
    int final_result = result.get<int>();

    int expected = 0;
    for (int i = 0; i < 100; ++i) {
        expected += i;
    }

    CHECK(final_result == expected);
}

TEST_CASE("TaskResult - Basic future functionality") {
    Pipeline pipeline;
    auto task = [](int input, TaskContext&) -> int { return input * 3; };
    auto result = pipeline.add_task<int, int>(task);

    SequentialExecutor executor;
    executor.execute(pipeline, 5);

    CHECK(result.get() == 15);
}

TEST_CASE("TaskResult - Multiple task futures") {
    Pipeline pipeline;

    auto add_task = [](int input, TaskContext&) -> int { return input + 10; };
    auto mul_task = [](int input, TaskContext&) -> int { return input * 2; };

    auto result1 = pipeline.add_task<int, int>(add_task);
    auto result2 = pipeline.add_task<int, int>(mul_task);
    pipeline.add_dependency(result1.id(), result2.id());

    SequentialExecutor executor;
    executor.execute(pipeline, 5);

    CHECK(result1.get() == 15);
    CHECK(result2.get() == 30);
}

TEST_CASE("TaskResult - Exception propagation") {
    Pipeline pipeline;

    auto throwing_task = [](int input, TaskContext&) -> int {
        if (input < 0) throw std::runtime_error("negative input");
        return input * 2;
    };

    auto result = pipeline.add_task<int, int>(throwing_task);

    SequentialExecutor executor;
    executor.execute(pipeline, -5);

    CHECK_THROWS_AS(result.get(), std::runtime_error);
}

TEST_CASE("TaskResult - Thread executor futures") {
    Pipeline pipeline;

    auto task = [](int input, TaskContext&) -> int {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return input * 4;
    };

    auto result = pipeline.add_task<int, int>(task);

    ThreadExecutor executor(2);
    executor.execute(pipeline, 7);

    CHECK(result.get() == 28);
}

TEST_CASE("TaskResult - Dynamic task futures") {
    Pipeline pipeline;
    std::vector<TaskResult<int>::Future> dynamic_futures;

    auto mapper = [&dynamic_futures](std::vector<int> input,
                                     TaskContext& ctx) -> int {
        int sum = 0;
        for (int val : input) {
            auto task_result = ctx.emit<int, int>(
                [](int x, TaskContext&) -> int { return x * x; }, Input{val});
            dynamic_futures.push_back(std::move(task_result.future()));
            sum += val;
        }
        return sum;
    };

    auto result = pipeline.add_task<std::vector<int>, int>(mapper);

    SequentialExecutor executor;
    std::vector<int> input = {2, 3, 4};
    executor.execute(pipeline, input);

    CHECK(result.get() == 9);
    CHECK(dynamic_futures.size() == 3);
    CHECK(dynamic_futures[0].get() == 4);
    CHECK(dynamic_futures[1].get() == 9);
    CHECK(dynamic_futures[2].get() == 16);
}

TEST_CASE("TaskResult - Mixed static and dynamic futures") {
    Pipeline pipeline;
    TaskResult<int>::Future emit_future;

    auto task = [&emit_future](int input, TaskContext& ctx) -> std::string {
        auto result = ctx.emit<int, int>(
            [](int x, TaskContext&) -> int { return x + 100; }, Input{input});
        emit_future = std::move(result.future());
        return "processed " + std::to_string(input);
    };

    auto static_result = pipeline.add_task<int, std::string>(task);

    SequentialExecutor executor;
    executor.execute(pipeline, 42);

    CHECK(static_result.get() == "processed 42");
    CHECK(emit_future.get() == 142);
}

TEST_CASE("TaskResult - Pipeline task exception handling") {
    SUBCASE("Sequential executor") {
        Pipeline pipeline;

        auto throwing_task = [](int input, TaskContext&) -> int {
            if (input < 0) {
                throw std::runtime_error("Pipeline task error: negative input");
            }
            return input * 2;
        };

        auto result = pipeline.add_task<int, int>(throwing_task);

        SequentialExecutor executor;
        executor.execute(pipeline, -10);

        CHECK_THROWS_WITH(result.get(), "Pipeline task error: negative input");
    }

    SUBCASE("Thread executor") {
        Pipeline pipeline;

        auto throwing_task = [](int input, TaskContext&) -> int {
            if (input < 0) {
                throw std::runtime_error("Pipeline task error: negative input");
            }
            return input * 2;
        };

        auto result = pipeline.add_task<int, int>(throwing_task);

        ThreadExecutor executor(2);
        executor.execute(pipeline, -10);

        CHECK_THROWS_WITH(result.get(), "Pipeline task error: negative input");
    }
}

TEST_CASE("TaskResult - Dynamic task exception handling") {
    SUBCASE("Sequential executor") {
        Pipeline pipeline;
        TaskResult<int>::Future dynamic_future;

        auto parent_task = [&dynamic_future](int input,
                                             TaskContext& ctx) -> int {
            auto throwing_child = [](int x, TaskContext&) -> int {
                if (x > 50) {
                    throw std::runtime_error(
                        "Dynamic task error: value too large");
                }
                return x * 2;
            };

            auto result = ctx.emit<int, int>(throwing_child, Input{input});
            dynamic_future = std::move(result.future());
            return input + 1;
        };

        auto parent_result = pipeline.add_task<int, int>(parent_task);

        SequentialExecutor executor;
        executor.execute(pipeline, 100);

        CHECK(parent_result.get() == 101);
        CHECK_THROWS_WITH(dynamic_future.get(),
                          "Dynamic task error: value too large");
    }

    SUBCASE("Thread executor") {
        Pipeline pipeline;
        TaskResult<int>::Future dynamic_future;

        auto parent_task = [&dynamic_future](int input,
                                             TaskContext& ctx) -> int {
            auto throwing_child = [](int x, TaskContext&) -> int {
                if (x > 50) {
                    throw std::runtime_error(
                        "Dynamic task error: value too large");
                }
                return x * 2;
            };

            auto result = ctx.emit<int, int>(throwing_child, Input{input});
            dynamic_future = std::move(result.future());
            return input + 1;
        };

        auto parent_result = pipeline.add_task<int, int>(parent_task);

        ThreadExecutor executor(2);
        executor.execute(pipeline, 100);

        CHECK(parent_result.get() == 101);
        CHECK_THROWS_WITH(dynamic_future.get(),
                          "Dynamic task error: value too large");
    }
}

TEST_CASE("TaskResult - Multiple dynamic tasks with exceptions") {
    SUBCASE("Sequential executor") {
        Pipeline pipeline;
        std::vector<TaskResult<int>::Future> dynamic_futures;

        auto parent_task = [&dynamic_futures](std::vector<int> input,
                                              TaskContext& ctx) -> int {
            int sum = 0;
            for (size_t i = 0; i < input.size(); ++i) {
                auto child = [i](int x, TaskContext&) -> int {
                    if (x < 0) {
                        throw std::runtime_error("Dynamic task " +
                                                 std::to_string(i) + " failed");
                    }
                    return x * x;
                };

                auto result = ctx.emit<int, int>(child, Input{input[i]});
                dynamic_futures.push_back(std::move(result.future()));
                sum += input[i];
            }
            return sum;
        };

        auto result = pipeline.add_task<std::vector<int>, int>(parent_task);

        SequentialExecutor executor;
        std::vector<int> input = {2, -3, 4};
        executor.execute(pipeline, input);

        CHECK(result.get() == 3);
        CHECK(dynamic_futures[0].get() == 4);
        CHECK_THROWS_WITH(dynamic_futures[1].get(), "Dynamic task 1 failed");
        CHECK(dynamic_futures[2].get() == 16);
    }

    SUBCASE("Thread executor") {
        Pipeline pipeline;
        std::vector<TaskResult<int>::Future> dynamic_futures;

        auto parent_task = [&dynamic_futures](std::vector<int> input,
                                              TaskContext& ctx) -> int {
            int sum = 0;
            for (size_t i = 0; i < input.size(); ++i) {
                auto child = [i](int x, TaskContext&) -> int {
                    if (x < 0) {
                        throw std::runtime_error("Dynamic task " +
                                                 std::to_string(i) + " failed");
                    }
                    return x * x;
                };

                auto result = ctx.emit<int, int>(child, Input{input[i]});
                dynamic_futures.push_back(std::move(result.future()));
                sum += input[i];
            }
            return sum;
        };

        auto result = pipeline.add_task<std::vector<int>, int>(parent_task);

        ThreadExecutor executor(2);
        std::vector<int> input = {2, -3, 4};
        executor.execute(pipeline, input);

        CHECK(result.get() == 3);
        CHECK(dynamic_futures[0].get() == 4);
        CHECK_THROWS_WITH(dynamic_futures[1].get(), "Dynamic task 1 failed");
        CHECK(dynamic_futures[2].get() == 16);
    }
}
