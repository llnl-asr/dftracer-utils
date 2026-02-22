Pipeline Guide
==============

The pipeline system provides a framework for building parallel data processing workflows using tasks, coroutines, and channels.

Overview
--------

The pipeline consists of:

- **Tasks**: Units of work that can depend on other tasks
- **Coroutines**: Async functions using C++20 ``co_await``/``co_return``
- **Channels**: Thread-safe queues for producer-consumer patterns
- **TaskGraph**: DAG builder for complex workflows

Basic Task Creation
-------------------

Tasks are created using ``make_task`` and return ``CoroTask<T>``:

.. code-block:: cpp

   #include <dftracer/utils/core/tasks/task.h>
   #include <dftracer/utils/core/coro/task.h>

   using namespace dftracer::utils;
   using namespace dftracer::utils::coro;

   // Simple task returning a value
   auto task = make_task([](TaskContext& ctx) -> CoroTask<int> {
       co_return 42;
   }, "MyTask");

   // Task with input from dependency
   auto processor = make_task([](TaskContext& ctx, Data input) -> CoroTask<Result> {
       co_return process(input);
   }, "Processor");
   processor->depends_on(source_task);

Pipeline Configuration
----------------------

Configure pipeline with ``PipelineConfig``:

.. code-block:: cpp

   #include <dftracer/utils/core/pipeline/pipeline.h>
   #include <dftracer/utils/core/pipeline/pipeline_config.h>

   auto config = PipelineConfig()
       .with_name("MyPipeline")
       .with_compute_threads(8)
       .with_watchdog(true)
       .with_global_timeout(std::chrono::seconds(300))
       .with_task_timeout(std::chrono::seconds(60));

   Pipeline pipeline(config);

Pipeline Execution
------------------

Run tasks through a Pipeline:

.. code-block:: cpp

   Pipeline pipeline(config);

   // Add tasks and set entry points
   pipeline.set_source(source_tasks);      // Vector of starting tasks
   pipeline.set_destination(final_task);   // Optional final task
   pipeline.execute();

   // Get results after execution
   auto result = final_task->get<ResultType>();

Coroutine Combinators
---------------------

Chain operations using combinators:

.. code-block:: cpp

   // Chain with then()
   auto result = co_await compute()
       .then([](int x) { return x * 2; })
       .then([](int x) { return std::to_string(x); });

   // Chain with operator>
   auto result = co_await compute()
       > [](int x) { return x * 2; }
       > [](int x) { return std::to_string(x); };

   // Side effects with tap()
   auto result = co_await compute()
       .tap([](int x) { log("value: {}", x); })
       .then([](int x) { return x * 2; });

   // Fallback with operator|
   auto result = co_await (primary() | fallback());

See :doc:`cpp_api/coro` for full API.

Producer-Consumer Pattern
-------------------------

Use ``Channel<T>`` for streaming data between tasks. This pattern is useful when multiple producers generate data consumed by a single writer.

.. code-block:: cpp

   #include <dftracer/utils/core/coro/channel.h>

   // Create channel with capacity
   auto channel = coro::make_channel<Batch>(100);

   // Multiple producer tasks
   std::vector<std::shared_ptr<Task>> producers;
   for (std::size_t i = 0; i < input_files.size(); ++i) {
       auto task = make_task(
           [i, &input_files, &channel](TaskContext& ctx) -> coro::CoroTask<void> {
               // RAII guard - channel closes when last producer exits
               auto guard = channel->producer_guard();

               // Read and send batches
                for (auto& batch : read_batches(input_files[i])) {
                    co_await channel->send(std::move(batch));
                }
               co_return;
           },
           "Producer-" + std::to_string(i));
       producers.push_back(task);
   }

   // Single consumer task
   auto consumer = make_task(
       [&channel, &output_file](TaskContext& ctx) -> coro::CoroTask<void> {
           while (auto batch = co_await channel->receive()) {
               write_batch(output_file, *batch);
           }
           co_return;
       },
       "Consumer");

   // Execute
   std::vector<std::shared_ptr<Task>> all_tasks = producers;
   all_tasks.push_back(consumer);
   pipeline.set_source(all_tasks);
   pipeline.execute();

Parallel Execution
------------------

Run multiple operations concurrently:

.. code-block:: cpp

   #include <dftracer/utils/core/coro/when_all.h>
   #include <dftracer/utils/core/coro/when_any.h>

   // Wait for all
   std::vector<IOAwaitable<Data>> ops;
   for (int i = 0; i < 100; i++) {
       ops.push_back(ctx.spawn_io([i]() { return read_chunk(i); }));
   }
   auto results = co_await when_all(std::move(ops));

   // Race (first wins)
   auto result = co_await when_any({
       ctx.spawn_io([]() { return read_cache(); }),
       ctx.spawn_io([]() { return read_disk(); })
   });
   // result.index tells which completed first

See :doc:`cpp_api/coro` for ``when_all``, ``when_any``, and ``timeout``.

Lazy Sequences
--------------

Generate values on-demand:

.. code-block:: cpp

   #include <dftracer/utils/core/coro/generator.h>

   // Synchronous generator
   Generator<int> range(int start, int end) {
       for (int i = start; i < end; ++i) {
           co_yield i;
       }
   }

   for (int x : range(0, 100)) {
       process(x);
   }

.. code-block:: cpp

   #include <dftracer/utils/core/coro/async_generator.h>

   // Async generator
   AsyncGenerator<Data> read_all(TaskContext& ctx) {
       for (auto& path : paths) {
           auto data = co_await ctx.spawn_io([&]() {
               return read_file(path);
           });
           co_yield data;
       }
   }

   auto gen = read_all(ctx);
   while (auto value = co_await gen.next()) {
       process(*value);
   }

TaskGraph for DAGs
------------------

Build complex task graphs with fan-out, fan-in, map, and reduce. Example from ``dftracer_split``:

.. code-block:: cpp

   #include <dftracer/utils/core/task_graph/task_graph.h>
   #include <dftracer/utils/core/pipeline/pipeline.h>

   using namespace dftracer::utils;
   using namespace dftracer::utils::task_graph;

   auto graph = TaskGraph::builder("DFTracerSplit");

   // Phase 1: Parallel file processing
   auto file_metadata = graph.parallel<Metadata>(
       input_files.size(),
       [&input_files](TaskContext&, std::size_t idx) -> coro::CoroTask<Metadata> {
           // Each task processes one file
           co_return collect_metadata(input_files[idx]);
       },
       "ProcessFile");

   // Phase 2: Reduce all metadata into chunk manifests
   auto manifests = graph.reduce<std::vector<Manifest>>(
       file_metadata, split_every{input_files.size()},
       [](TaskContext&, std::vector<Metadata> all) -> coro::CoroTask<std::vector<Manifest>> {
           co_return create_manifests(all);
       },
       "CreateManifests");

   // Phase 3: Create extraction task with combiner
   auto extractor = make_task(...);
   extractor->depends_on(manifests.task());
   graph.add(extractor);

   // Execute pipeline
   Pipeline pipeline(config);
   pipeline.set_source(file_metadata.tasks());
   pipeline.set_destination(extractor);
   pipeline.execute();

   // Get results
   auto results = extractor->get<std::vector<Result>>();

Common patterns:

.. code-block:: cpp

   // Fan-out: 1 -> N
   auto workers = graph.fan_out<Result>(source, num_outputs{4},
       [](TaskContext& ctx, Data input, std::size_t idx) -> coro::CoroTask<Result> {
           co_return process_shard(input, idx);
       }, "Worker");

   // Fan-in: M -> 1
   auto combined = graph.fan_in<Result>(workers,
       [](TaskContext& ctx, std::vector<Result> inputs) -> coro::CoroTask<Result> {
           co_return combine(inputs);
       }, "Combine");

   // Map: 1-to-1 transform
   auto transformed = graph.map<Output>(inputs,
       [](TaskContext& ctx, Input in) -> coro::CoroTask<Output> {
           co_return transform(in);
       }, "Transform");

See :doc:`cpp_api/task_graph` for full API.

API Reference
-------------

- :doc:`cpp_api/coro` - CoroTask, Channel, Generator, when_all, when_any
- :doc:`cpp_api/task_graph` - TaskGraph, TaskGroup, factory functions
- :doc:`cpp_api/pipeline` - Pipeline, Executor, Task classes
