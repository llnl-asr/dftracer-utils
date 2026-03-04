#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/task_graph/reduction.h>
#include <dftracer/utils/core/task_graph/task_graph.h>
#include <dftracer/utils/core/task_graph/task_group.h>
#include <dftracer/utils/core/task_graph/task_result.h>
#include <dftracer/utils/core/task_graph/types.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <doctest/doctest.h>

#include <memory>
#include <numeric>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::task_graph;

// ============================================================================
// Strong Types Tests
// ============================================================================

TEST_CASE("split_every - construction") {
    split_every s{2};
    CHECK(s.count == 2);
}

TEST_CASE("num_outputs - construction") {
    num_outputs n{8};
    CHECK(n.count == 8);
}

// ============================================================================
// TaskResult Tests
// ============================================================================

TEST_CASE("TaskResult - make and get") {
    auto result = task_graph::TaskResult<int>::make(42);
    CHECK(result.is_ready());
    CHECK(!result.empty());
    CHECK(result.get() == 42);
}

TEST_CASE("TaskResult - copy") {
    auto result = task_graph::TaskResult<std::string>::make("hello");
    auto copied = result.copy();
    CHECK(copied == "hello");
    CHECK(result.get() == "hello");
}

TEST_CASE("TaskResult - share") {
    auto result = task_graph::TaskResult<int>::make(100);
    auto shared = result.share();
    CHECK(*shared == 100);
}

TEST_CASE("TaskResult - from_shared") {
    auto ptr = std::make_shared<int>(999);
    auto result = task_graph::TaskResult<int>::from_shared(ptr);
    CHECK(result.get() == 999);
}

TEST_CASE("TaskResult - empty result throws on get") {
    task_graph::TaskResult<int> empty;
    CHECK(empty.empty());
    CHECK(!empty.is_ready());
    CHECK_THROWS_AS(empty.get(), std::runtime_error);
}

TEST_CASE("task_graph::TaskResult<void> - make") {
    auto result = task_graph::TaskResult<void>::make();
    CHECK(result.is_ready());
    CHECK(!result.empty());
}

TEST_CASE("TaskResult - size_bytes for simple type") {
    auto result = task_graph::TaskResult<int>::make(42);
    CHECK(result.size_bytes() >= sizeof(int));
}

TEST_CASE("TaskResult - size_bytes for vector") {
    std::vector<int> vec{1, 2, 3, 4, 5};
    auto result =
        task_graph::TaskResult<std::vector<int>>::make(std::move(vec));
    CHECK(result.size_bytes() > sizeof(std::vector<int>));
}

// ============================================================================
// TaskGroup Tests
// ============================================================================

TEST_CASE("TaskGroup - empty construction") {
    TaskGroup<int> group;
    CHECK(group.empty());
    CHECK(group.size() == 0);
}

TEST_CASE("TaskGroup - from single task") {
    auto task = make_task(
        [](CoroScope&) -> coro::CoroTask<int> { co_return 42; }, "Test");
    TaskGroup<int> group(task);
    CHECK(group.size() == 1);
    CHECK(group.task() == task);
}

TEST_CASE("TaskGroup - from vector") {
    std::vector<std::shared_ptr<Task>> tasks;
    tasks.push_back(make_task(
        [](CoroScope&) -> coro::CoroTask<int> { co_return 1; }, "T1"));
    tasks.push_back(make_task(
        [](CoroScope&) -> coro::CoroTask<int> { co_return 2; }, "T2"));

    TaskGroup<int> group(tasks);
    CHECK(group.size() == 2);
    CHECK_THROWS_AS(group.task(), std::runtime_error);
}

TEST_CASE("TaskGroup - add and iterate") {
    TaskGroup<int> group;
    group.add(make_task([](CoroScope&) -> coro::CoroTask<int> { co_return 1; },
                        "T1"));
    group.add(make_task([](CoroScope&) -> coro::CoroTask<int> { co_return 2; },
                        "T2"));

    CHECK(group.size() == 2);

    int count = 0;
    for ([[maybe_unused]] const auto& task : group) {
        ++count;
    }
    CHECK(count == 2);
}

// ============================================================================
// Reduction Algorithm Tests
// ============================================================================

TEST_CASE("partition_all - even division") {
    std::vector<int> items{0, 1, 2, 3, 4, 5};
    auto result = partition_all(2, items);

    CHECK(result.size() == 3);
    CHECK(result[0] == std::vector<int>{0, 1});
    CHECK(result[1] == std::vector<int>{2, 3});
    CHECK(result[2] == std::vector<int>{4, 5});
}

TEST_CASE("partition_all - odd handling (Dask-style)") {
    std::vector<int> items{0, 1, 2, 3, 4, 5, 6};
    auto result = partition_all(2, items);

    CHECK(result.size() == 4);
    CHECK(result[0] == std::vector<int>{0, 1});
    CHECK(result[1] == std::vector<int>{2, 3});
    CHECK(result[2] == std::vector<int>{4, 5});
    CHECK(result[3] == std::vector<int>{6});
}

TEST_CASE("partition_all - larger groups") {
    std::vector<int> items{0, 1, 2, 3, 4};
    auto result = partition_all(3, items);

    CHECK(result.size() == 2);
    CHECK(result[0] == std::vector<int>{0, 1, 2});
    CHECK(result[1] == std::vector<int>{3, 4});
}

TEST_CASE("partition_all - empty input") {
    std::vector<int> items;
    auto result = partition_all(2, items);
    CHECK(result.empty());
}

TEST_CASE("partition_all - throws on n=0") {
    std::vector<int> items{1, 2, 3};
    CHECK_THROWS_AS(partition_all(0, items), std::invalid_argument);
}

TEST_CASE("tree_reduction_depth - various sizes") {
    CHECK(tree_reduction_depth(1, 2) == 0);  // 1 item -> 0 levels
    CHECK(tree_reduction_depth(2, 2) == 1);
    CHECK(tree_reduction_depth(4, 2) == 2);
    CHECK(tree_reduction_depth(7, 2) == 3);  // 7 -> 4 -> 2 -> 1
    CHECK(tree_reduction_depth(8, 2) == 3);
}

TEST_CASE("tree_reduction_levels - 7 items") {
    auto levels = tree_reduction_levels(7, 2);

    CHECK(levels.size() == 3);
    CHECK(levels[0] == 4);  // ceil(7/2)
    CHECK(levels[1] == 2);  // ceil(4/2)
    CHECK(levels[2] == 1);  // ceil(2/2)
}

TEST_CASE("build_tree_indices - 7 items") {
    auto levels = build_tree_indices(7, 2);

    CHECK(levels.size() == 3);

    // Level 0: [[0,1], [2,3], [4,5], [6]]
    CHECK(levels[0].size() == 4);
    CHECK(levels[0][0] == std::vector<std::size_t>{0, 1});
    CHECK(levels[0][1] == std::vector<std::size_t>{2, 3});
    CHECK(levels[0][2] == std::vector<std::size_t>{4, 5});
    CHECK(levels[0][3] == std::vector<std::size_t>{6});

    // Level 1: [[0,1], [2,3]]
    CHECK(levels[1].size() == 2);
    CHECK(levels[1][0] == std::vector<std::size_t>{0, 1});
    CHECK(levels[1][1] == std::vector<std::size_t>{2, 3});

    // Level 2: [[0,1]]
    CHECK(levels[2].size() == 1);
    CHECK(levels[2][0] == std::vector<std::size_t>{0, 1});
}

// ============================================================================
// TaskGraph Builder Tests
// ============================================================================

TEST_CASE("TaskGraph - builder without pipeline") {
    auto graph = TaskGraph::builder("TestGraph");
    CHECK(graph.name() == "TestGraph");
    CHECK(graph.tasks().empty());
}

TEST_CASE("TaskGraph - parallel creates N tasks") {
    auto graph = TaskGraph::builder("Test");

    auto group = graph.parallel<task_graph::TaskResult<int>>(
        4,
        [](CoroScope&,
           std::size_t id) -> coro::CoroTask<task_graph::TaskResult<int>> {
            co_return task_graph::TaskResult<int>::make(static_cast<int>(id));
        },
        "Worker");

    CHECK(group.size() == 4);
    CHECK(graph.tasks().size() == 4);
}

TEST_CASE("TaskGraph - parallel tasks in graph") {
    auto graph = TaskGraph::builder("Test");

    auto group = graph.parallel<task_graph::TaskResult<int>>(
        4,
        [](CoroScope&,
           std::size_t id) -> coro::CoroTask<task_graph::TaskResult<int>> {
            co_return task_graph::TaskResult<int>::make(static_cast<int>(id));
        },
        "Worker");

    CHECK(group.size() == 4);
    CHECK(graph.tasks().size() == 4);
}

TEST_CASE("TaskGraph - wrap external task") {
    auto external = make_task(
        [](CoroScope&) -> coro::CoroTask<int> { co_return 100; }, "External");

    auto graph = TaskGraph::builder("Test");
    auto wrapped = graph.wrap<int>(external);

    CHECK(wrapped.size() == 1);
    CHECK(wrapped.task() == external);
}

// ============================================================================
// Integration Test - Full Pipeline
// ============================================================================

TEST_CASE("TaskGraph - parallel + reduce integration") {
    auto graph = TaskGraph::builder("MapReduce");

    auto workers = graph.parallel<int>(
        8,
        [](CoroScope&, std::size_t id) -> coro::CoroTask<int> {
            co_return static_cast<int>(id + 1);
        },
        "Worker");

    CHECK(workers.size() == 8);

    auto reduced = graph.reduce<int>(
        workers, split_every{2},
        [](CoroScope&, std::vector<int> items) -> coro::CoroTask<int> {
            int sum = 0;
            for (int x : items) sum += x;
            co_return sum;
        },
        "Sum");

    CHECK(reduced.size() == 1);

    Pipeline pipeline(PipelineConfig::parallel(4));
    pipeline.set_source(workers.tasks());
    pipeline.execute();

    auto result = reduced.task()->get<int>();
    CHECK(result == 36);  // 1+2+...+8
}

TEST_CASE("TaskGraph - barebone task as input (before graph)") {
    auto source_task =
        make_task([](CoroScope&) -> coro::CoroTask<int> { co_return 10; },
                  "BareboneSource");

    auto graph = TaskGraph::builder("WithBareboneInput");
    auto wrapped = graph.wrap<int>(source_task);

    auto mapped = graph.map<int>(
        wrapped,
        [](CoroScope&, int value) -> coro::CoroTask<int> {
            co_return value * 2;
        },
        "Double");

    CHECK(mapped.size() == 1);

    Pipeline pipeline(PipelineConfig::parallel(2));
    pipeline.set_source(source_task);
    pipeline.execute();

    auto result = mapped.task()->get<int>();
    CHECK(result == 20);  // 10 * 2
}

TEST_CASE("TaskGraph - barebone task consumes graph output (after graph)") {
    auto graph = TaskGraph::builder("GraphWithBareboneOutput");

    auto workers = graph.parallel<int>(
        4,
        [](CoroScope&, std::size_t id) -> coro::CoroTask<int> {
            co_return static_cast<int>(id + 1);
        },
        "Worker");

    auto reduced = graph.reduce<int>(
        workers, split_every{2},
        [](CoroScope&, std::vector<int> items) -> coro::CoroTask<int> {
            int sum = 0;
            for (int x : items) sum += x;
            co_return sum;
        },
        "Sum");

    auto consumer_task = make_task(
        [](CoroScope&, int graph_result) -> coro::CoroTask<std::string> {
            co_return "Result: " + std::to_string(graph_result);
        },
        "BareboneConsumer");

    consumer_task->depends_on(reduced.task());

    Pipeline pipeline(PipelineConfig::parallel(2));
    pipeline.set_source(workers.tasks());
    pipeline.execute();

    auto result = consumer_task->get<std::string>();
    CHECK(result == "Result: 10");  // 1+2+3+4
}

TEST_CASE("TaskGraph - reduce with split_every > 2") {
    auto graph = TaskGraph::builder("SplitEvery3");

    auto workers = graph.parallel<int>(
        9,
        [](CoroScope&, std::size_t id) -> coro::CoroTask<int> {
            co_return static_cast<int>(id + 1);
        },
        "Worker");

    CHECK(workers.size() == 9);

    auto reduced = graph.reduce<int>(
        workers, split_every{3},
        [](CoroScope&, std::vector<int> items) -> coro::CoroTask<int> {
            int sum = 0;
            for (int x : items) sum += x;
            co_return sum;
        },
        "Sum");

    CHECK(reduced.size() == 1);

    Pipeline pipeline(PipelineConfig::parallel(4));
    pipeline.set_source(workers.tasks());
    pipeline.execute();

    auto result = reduced.task()->get<int>();
    CHECK(result == 45);  // 1+2+...+9
}

TEST_CASE("TaskGraph - fold with init value") {
    auto graph = TaskGraph::builder("FoldTest");

    auto workers = graph.parallel<int>(
        5,
        [](CoroScope&, std::size_t id) -> coro::CoroTask<int> {
            co_return static_cast<int>(id + 1);
        },
        "Worker");

    CHECK(workers.size() == 5);

    // Fold with init=0, binary op=add
    auto folded = graph.fold<int>(
        workers, 0, split_every{2}, [](int acc, int x) { return acc + x; },
        "Sum");

    CHECK(folded.size() == 1);

    Pipeline pipeline(PipelineConfig::parallel(4));
    pipeline.set_source(workers.tasks());
    pipeline.execute();

    auto result = folded.task()->get<int>();
    CHECK(result == 15);  // 1+2+3+4+5
}

TEST_CASE("TaskGraph - fold with product") {
    auto graph = TaskGraph::builder("FoldProduct");

    auto workers = graph.parallel<int>(
        4,
        [](CoroScope&, std::size_t id) -> coro::CoroTask<int> {
            co_return static_cast<int>(id + 1);
        },
        "Worker");

    // Fold with init=1, binary op=multiply
    auto folded = graph.fold<int>(
        workers, 1, split_every{2}, [](int acc, int x) { return acc * x; },
        "Product");

    Pipeline pipeline(PipelineConfig::parallel(2));
    pipeline.set_source(workers.tasks());
    pipeline.execute();

    auto result = folded.task()->get<int>();
    CHECK(result == 24);  // 1*2*3*4
}

TEST_CASE("TaskGraph - aggregate (map + reduce)") {
    auto graph = TaskGraph::builder("AggregateTest");

    auto workers = graph.parallel<int>(
        4,
        [](CoroScope&, std::size_t id) -> coro::CoroTask<int> {
            co_return static_cast<int>(id + 1);
        },
        "Worker");

    // Aggregate: square each value, then sum
    auto aggregated = graph.aggregate<int, int>(
        workers,
        [](CoroScope&, int x) -> coro::CoroTask<int> { co_return x* x; },
        split_every{2},
        [](CoroScope&, std::vector<int> items) -> coro::CoroTask<int> {
            int sum = 0;
            for (int x : items) sum += x;
            co_return sum;
        },
        "SumOfSquares");

    Pipeline pipeline(PipelineConfig::parallel(2));
    pipeline.set_source(workers.tasks());
    pipeline.execute();

    auto result = aggregated.task()->get<int>();
    CHECK(result == 30);  // 1^2 + 2^2 + 3^2 + 4^2 = 1+4+9+16
}

TEST_CASE("TaskGraph - aggregate with type transformation") {
    auto graph = TaskGraph::builder("AggregateTypeChange");

    auto workers = graph.parallel<std::string>(
        3,
        [](CoroScope&, std::size_t id) -> coro::CoroTask<std::string> {
            co_return std::to_string(id + 1);
        },
        "Worker");

    // Aggregate: convert string to int, then sum
    auto aggregated = graph.aggregate<int, int>(
        workers,
        [](CoroScope&, std::string s) -> coro::CoroTask<int> {
            co_return std::stoi(s);
        },
        split_every{2},
        [](CoroScope&, std::vector<int> items) -> coro::CoroTask<int> {
            int sum = 0;
            for (int x : items) sum += x;
            co_return sum;
        },
        "StringToSum");

    Pipeline pipeline(PipelineConfig::parallel(2));
    pipeline.set_source(workers.tasks());
    pipeline.execute();

    auto result = aggregated.task()->get<int>();
    CHECK(result == 6);  // 1+2+3
}

TEST_CASE("TaskGraph - barebone tasks on both ends") {
    auto source =
        make_task([](CoroScope&) -> coro::CoroTask<int> { co_return 5; },
                  "BareboneStart");

    auto graph = TaskGraph::builder("FullIntegration");
    auto wrapped = graph.wrap<int>(source);

    auto fanned = graph.fan_out<int>(
        wrapped, num_outputs{4},
        [](CoroScope&, int input, std::size_t idx) -> coro::CoroTask<int> {
            co_return input* static_cast<int>(idx + 1);
        },
        "Multiply");

    auto reduced = graph.reduce<int>(
        fanned, split_every{2},
        [](CoroScope&, std::vector<int> items) -> coro::CoroTask<int> {
            int sum = 0;
            for (int x : items) sum += x;
            co_return sum;
        },
        "Sum");

    auto consumer = make_task(
        [](CoroScope&, int value) -> coro::CoroTask<int> {
            co_return value * 10;
        },
        "BareboneEnd");
    consumer->depends_on(reduced.task());

    Pipeline pipeline(PipelineConfig::parallel(2));
    pipeline.set_source(source);
    pipeline.execute();

    auto result = consumer->get<int>();
    CHECK(result == 500);  // (5*1 + 5*2 + 5*3 + 5*4) * 10
}

// ============================================================================
// Partition Tests
// ============================================================================

TEST_CASE("num_partitions - construction") {
    num_partitions n{4};
    CHECK(n.count == 4);
}

TEST_CASE("TaskGraph - partition basic") {
    std::vector<int> data = {1, 2, 3, 4, 5, 6, 7, 8};

    auto graph = TaskGraph::builder("PartitionBasic");
    auto parts = graph.partition<int>(data, num_partitions{4});

    CHECK(parts.size() == 4);
    CHECK(graph.tasks().size() == 4);

    Pipeline pipeline(PipelineConfig::parallel(2));
    pipeline.set_source(parts.tasks());
    pipeline.execute();

    auto chunk0 = parts[0]->get<std::vector<int>>();
    auto chunk1 = parts[1]->get<std::vector<int>>();
    auto chunk2 = parts[2]->get<std::vector<int>>();
    auto chunk3 = parts[3]->get<std::vector<int>>();

    CHECK(chunk0 == std::vector<int>{1, 2});
    CHECK(chunk1 == std::vector<int>{3, 4});
    CHECK(chunk2 == std::vector<int>{5, 6});
    CHECK(chunk3 == std::vector<int>{7, 8});
}

TEST_CASE("TaskGraph - partition uneven split") {
    std::vector<int> data = {1, 2, 3, 4, 5, 6, 7};

    auto graph = TaskGraph::builder("PartitionUneven");
    auto parts = graph.partition<int>(data, num_partitions{3});

    CHECK(parts.size() == 3);

    Pipeline pipeline(PipelineConfig::parallel(2));
    pipeline.set_source(parts.tasks());
    pipeline.execute();

    auto chunk0 = parts[0]->get<std::vector<int>>();
    auto chunk1 = parts[1]->get<std::vector<int>>();
    auto chunk2 = parts[2]->get<std::vector<int>>();

    CHECK(chunk0 == std::vector<int>{1, 2, 3});  // 3 items (gets extra)
    CHECK(chunk1 == std::vector<int>{4, 5});     // 2 items
    CHECK(chunk2 == std::vector<int>{6, 7});     // 2 items
}

TEST_CASE("TaskGraph - partition + map") {
    std::vector<int> data = {1, 2, 3, 4, 5, 6};

    auto graph = TaskGraph::builder("PartitionMap");
    auto parts = graph.partition<int>(data, num_partitions{3});

    auto mapped = graph.map<int>(
        parts, [](CoroScope&, std::vector<int> chunk) -> coro::CoroTask<int> {
            int sum = 0;
            for (int x : chunk) sum += x;
            co_return sum;
        });

    CHECK(mapped.size() == 3);

    Pipeline pipeline(PipelineConfig::parallel(2));
    pipeline.set_source(parts.tasks());
    pipeline.execute();

    auto sum0 = mapped[0]->get<int>();
    auto sum1 = mapped[1]->get<int>();
    auto sum2 = mapped[2]->get<int>();

    CHECK(sum0 == 3);   // 1+2
    CHECK(sum1 == 7);   // 3+4
    CHECK(sum2 == 11);  // 5+6
}

TEST_CASE("TaskGraph - partition + reduce") {
    std::vector<int> data = {1, 2, 3, 4, 5, 6, 7, 8};

    auto graph = TaskGraph::builder("PartitionReduce");
    auto parts = graph.partition<int>(data, num_partitions{4});

    auto sums = graph.map<int>(
        parts, [](CoroScope&, std::vector<int> chunk) -> coro::CoroTask<int> {
            int sum = 0;
            for (int x : chunk) sum += x;
            co_return sum;
        });

    auto total = graph.reduce<int>(
        sums, split_every{2},
        [](CoroScope&, std::vector<int> items) -> coro::CoroTask<int> {
            int sum = 0;
            for (int x : items) sum += x;
            co_return sum;
        });

    Pipeline pipeline(PipelineConfig::parallel(2));
    pipeline.set_source(parts.tasks());
    pipeline.execute();

    auto result = total.task()->get<int>();
    CHECK(result == 36);  // 1+2+...+8
}

TEST_CASE("TaskGraph - concat_partitions") {
    std::vector<int> data = {1, 2, 3, 4, 5, 6, 7, 8};

    auto graph = TaskGraph::builder("ConcatPartitions");
    auto parts = graph.partition<int>(data, num_partitions{4});

    auto combined = graph.concat_partitions(parts);

    CHECK(combined.size() == 1);

    Pipeline pipeline(PipelineConfig::parallel(2));
    pipeline.set_source(parts.tasks());
    pipeline.execute();

    auto result = combined.task()->get<std::vector<int>>();
    CHECK(result == data);
}

TEST_CASE("TaskGraph - partition + map + concat_partitions") {
    std::vector<int> data = {1, 2, 3, 4};

    auto graph = TaskGraph::builder("PartitionMapConcat");
    auto parts = graph.partition<int>(data, num_partitions{2});

    auto doubled = graph.map<std::vector<int>>(
        parts,
        [](CoroScope&,
           std::vector<int> chunk) -> coro::CoroTask<std::vector<int>> {
            for (auto& x : chunk) x *= 2;
            co_return chunk;
        });

    auto combined = graph.concat_partitions(doubled);

    Pipeline pipeline(PipelineConfig::parallel(2));
    pipeline.set_source(parts.tasks());
    pipeline.execute();

    auto result = combined.task()->get<std::vector<int>>();
    CHECK(result == std::vector<int>{2, 4, 6, 8});
}

TEST_CASE("TaskGraph - partition single element") {
    std::vector<int> data = {42};

    auto graph = TaskGraph::builder("PartitionSingle");
    auto parts = graph.partition<int>(data, num_partitions{1});

    CHECK(parts.size() == 1);

    Pipeline pipeline(PipelineConfig::parallel(1));
    pipeline.set_source(parts.tasks());
    pipeline.execute();

    auto result = parts[0]->get<std::vector<int>>();
    CHECK(result == std::vector<int>{42});
}

TEST_CASE("TaskGraph - partition empty data") {
    std::vector<int> data;

    auto graph = TaskGraph::builder("PartitionEmpty");
    auto parts = graph.partition<int>(data, num_partitions{2});

    CHECK(parts.size() == 2);

    Pipeline pipeline(PipelineConfig::parallel(1));
    pipeline.set_source(parts.tasks());
    pipeline.execute();

    auto chunk0 = parts[0]->get<std::vector<int>>();
    auto chunk1 = parts[1]->get<std::vector<int>>();

    CHECK(chunk0.empty());
    CHECK(chunk1.empty());
}

TEST_CASE("TaskGraph - reduce single element with type conversion") {
    auto graph = TaskGraph::builder("ReduceSingleTypeConvert");

    // Single parallel task producing a string
    auto workers = graph.parallel<std::string>(
        1,
        [](CoroScope&, std::size_t) -> coro::CoroTask<std::string> {
            co_return std::string("42");
        },
        "Worker");

    CHECK(workers.size() == 1);

    // Reduce: string -> int (type conversion must happen even with 1 input)
    auto reduced = graph.reduce<int>(
        workers, split_every{2},
        [](CoroScope&, std::vector<std::string> items) -> coro::CoroTask<int> {
            int sum = 0;
            for (const auto& s : items) sum += std::stoi(s);
            co_return sum;
        },
        "ParseAndSum");

    CHECK(reduced.size() == 1);
    // The reduce task must be a DIFFERENT task from the worker
    CHECK(reduced.task() != workers.task());

    Pipeline pipeline(PipelineConfig::parallel(2));
    pipeline.set_source(workers.tasks());
    pipeline.execute();

    auto result = reduced.task()->get<int>();
    CHECK(result == 42);
}

TEST_CASE("TaskGraph - reduce single element same type") {
    auto graph = TaskGraph::builder("ReduceSingleSameType");

    auto workers = graph.parallel<int>(
        1, [](CoroScope&, std::size_t) -> coro::CoroTask<int> { co_return 99; },
        "Worker");

    CHECK(workers.size() == 1);

    auto reduced = graph.reduce<int>(
        workers, split_every{2},
        [](CoroScope&, std::vector<int> items) -> coro::CoroTask<int> {
            int sum = 0;
            for (int x : items) sum += x;
            co_return sum;
        },
        "Sum");

    CHECK(reduced.size() == 1);
    // Even with same type, reduce should create a new task
    CHECK(reduced.task() != workers.task());

    Pipeline pipeline(PipelineConfig::parallel(2));
    pipeline.set_source(workers.tasks());
    pipeline.execute();

    auto result = reduced.task()->get<int>();
    CHECK(result == 99);
}
