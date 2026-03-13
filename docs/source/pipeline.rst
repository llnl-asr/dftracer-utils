Pipeline Guide
==============

The pipeline system provides a framework for building parallel data processing workflows using structured concurrency with coroutines, channels, and the executor model.

Overview
--------

The pipeline consists of:

- **Executor**: Thread pool managing ``CoroTask`` execution with work-stealing
- **CoroScope**: Lightweight structured concurrency context for spawning tasks
- **Coroutines**: Async functions using C++20 ``co_await``/``co_return``
- **Channels**: Thread-safe queues for producer-consumer patterns
- **JoinHandle**: Stack-allocated join barrier for coordinating parallel work

Basic Task Creation
-------------------

Tasks are created using ``make_task`` and receive a ``CoroScope&`` parameter:

.. code-block:: cpp

   #include <dftracer/utils/core/tasks/task.h>
   #include <dftracer/utils/core/coro/task.h>

   using namespace dftracer::utils;
   using namespace dftracer::utils::coro;

   // Simple task returning a value
   auto task = make_task([](CoroScope& scope) -> CoroTask<void> {
       co_return;
   }, "MyTask");

   // Task that spawns child work
   auto parent = make_task([](CoroScope& scope) -> CoroTask<void> {
       scope.spawn([](CoroScope& child_scope) -> CoroTask<void> {
           // Do work here
           co_return;
       });
       co_return;
   }, "ParentTask");

Pipeline Configuration and Execution
-------------------------------------

Configure pipeline with ``PipelineConfig``:

.. code-block:: cpp

   #include <dftracer/utils/core/pipeline/pipeline.h>
   #include <dftracer/utils/core/pipeline/pipeline_config.h>

   auto config = PipelineConfig()
       .with_name("MyPipeline")
       .with_compute_threads(8)              // Worker threads for executor
       .with_watchdog(true)                   // Enable hang detection
       .with_global_timeout(std::chrono::seconds(300))
       .with_task_timeout(std::chrono::seconds(60));

   Pipeline pipeline(config);

Key configuration options:

- ``with_compute_threads(N)`` - Number of worker threads (default: ``std::thread::hardware_concurrency()``)
- ``with_watchdog(bool)`` - Enable/disable watchdog for detecting hangs
- ``with_global_timeout(duration)`` - Maximum total execution time
- ``with_task_timeout(duration)`` - Per-task timeout
- ``with_executor_idle_timeout(duration)`` - Idle timeout before shutdown (default: 300s)
- ``with_executor_deadlock_timeout(duration)`` - Deadlock detection timeout (default: 600s)

Pipeline Execution
-------------------

Run tasks through a Pipeline:

.. code-block:: cpp

   Pipeline pipeline(config);

   // Create and add tasks
   std::vector<std::shared_ptr<Task>> source_tasks = {task1, task2};
   
   pipeline.set_source(source_tasks);      // Vector of starting tasks
   pipeline.set_destination(final_task);   // Optional final task
   pipeline.execute();                     // Block until complete

   // Get results after execution
   auto result = final_task->get<ResultType>();

CoroScope and Structured Concurrency
-------------------------------------

``CoroScope`` is the primary context type for managing concurrent work. It provides:

- ``spawn(lambda)`` - Fire-and-forget void coroutines
- ``spawn(lambda)`` returning ``SpawnFuture<T>`` - Typed results with ``co_await``
- ``scope(lambda)`` - Create a child scope (all spawned work must complete before returning)
- ``join_all()`` - Wait for all spawned work to complete

Example with parallel work:

.. code-block:: cpp

   #include <dftracer/utils/core/tasks/coro_scope.h>

   auto task = make_task([](CoroScope& scope) -> CoroTask<void> {
       // Spawn multiple parallel tasks
       for (std::size_t i = 0; i < 10; ++i) {
           scope.spawn([i](CoroScope& child) -> CoroTask<void> {
               // Process item i in parallel
               co_return;
           });
       }
       
       // Wait for all spawned tasks to complete
       co_await scope.join_all();
       
       co_return;
   });

Create a child scope for structured concurrency:

.. code-block:: cpp

   auto task = make_task([](CoroScope& scope) -> CoroTask<void> {
       // Create child scope - all work spawned in the lambda must complete
       // before the scope exits
       co_await scope.scope([](CoroScope& child_scope) -> CoroTask<void> {
           for (std::size_t i = 0; i < 10; ++i) {
               child_scope.spawn([i](CoroScope& s) -> CoroTask<void> {
                   co_return;
               });
           }
           // Implicit join_all() when this lambda returns
           co_return;
       });
       
       // All spawned work from the child scope is guaranteed complete here
       co_return;
   });

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

Producer-Consumer Pattern with Channels
-----------------------------------------

Use ``Channel<T>`` for streaming data between tasks. This pattern is useful when multiple producers generate data consumed by a single writer. The channel automatically closes when all producers exit.

.. code-block:: cpp

   #include <dftracer/utils/core/coro/channel.h>
   #include <dftracer/utils/core/tasks/task.h>

   // Create channel with capacity
   auto channel = coro::make_channel<Batch>(100);

   // Multiple producer tasks
   std::vector<std::shared_ptr<Task>> producers;
   for (std::size_t i = 0; i < input_files.size(); ++i) {
       auto task = make_task(
           [i, channel, input_files](CoroScope& scope) -> coro::CoroTask<void> {
               // RAII guard - auto-registers and releases producer slot
               auto guard = channel->producer_guard();

               // Read and send batches
               for (auto& batch : read_batches(input_files[i])) {
                   co_await channel->send(std::move(batch));
               }
               // ~ProducerGuard auto-releases; channel closes when last exits
               co_return;
           },
           "Producer-" + std::to_string(i));
       producers.push_back(task);
   }

   // Single consumer task
   auto consumer = make_task(
       [channel](CoroScope& scope) -> coro::CoroTask<void> {
           while (auto batch = co_await channel->receive()) {
               write_batch(*batch);
           }
           // receive() returns std::nullopt when channel closes
           co_return;
       },
       "Consumer");

   // Execute
   std::vector<std::shared_ptr<Task>> all_tasks = producers;
   all_tasks.push_back(consumer);
   Pipeline pipeline(config);
   pipeline.set_source(all_tasks);
   pipeline.execute();

**Key patterns:**

- ``channel->producer_guard()`` - RAII registration (increment count, auto-decrement on exit)
- ``channel->register_producer()`` / ``channel->adopt_producer()`` - Manual registration for pre-setup patterns
- ``co_await channel->send(T)`` - Enqueue value (may suspend if buffer full)
- ``co_await channel->receive()`` - Dequeue value (returns ``std::optional<T>``, nullopt when closed)

Fan-Out and Fan-In Patterns
----------------------------

Spawn multiple workers processing different work items in parallel, then collect results.

**Fan-Out:** One task spawns N parallel tasks:

.. code-block:: cpp

   auto task = make_task([](CoroScope& scope) -> CoroTask<void> {
       std::vector<SpawnFuture<Result>> futures;
       
       // Spawn N worker tasks
       for (std::size_t i = 0; i < 10; ++i) {
           auto future = scope.spawn([i](CoroScope& s) -> CoroTask<Result> {
               // Process item i
               co_return process_item(i);
           });
           futures.push_back(std::move(future));
       }
       
       // Collect results
       for (auto& future : futures) {
           Result r = co_await future;
           aggregate_result(r);
       }
       
       co_return;
   });

**Fan-In with Child Scope:** Spawn parallel work that must complete before proceeding:

.. code-block:: cpp

   auto task = make_task([](CoroScope& scope) -> CoroTask<void> {
       std::vector<Result> results;
       
       co_await scope.scope([&results](CoroScope& child) -> CoroTask<void> {
           // All spawned work below must complete before this lambda returns
           for (std::size_t i = 0; i < 10; ++i) {
               child.spawn([i, &results](CoroScope& s) -> CoroTask<void> {
                   Result r = process_item(i);
                   // Note: racey if writing to results vector!
                   // Use mutex or thread-safe container
                   co_return;
               });
           }
           co_return;
       });
       
       // All work is guaranteed complete here
       co_return;
   });

**Real-world example from dftracer_stats** - parallel chunk processing with shared results:

.. code-block:: cpp

   auto detailed = std::make_shared<DetailedStatistics>();
   auto chunk_mutex = std::make_shared<std::mutex>();
   
   co_await scope.scope([detailed, chunk_mutex, candidates](
       CoroScope& child) -> CoroTask<void> {
       for (auto ckpt_idx : candidates) {
           child.spawn(
               [ckpt_idx, detailed, chunk_mutex](CoroScope& s) -> CoroTask<void> {
                   auto scan_output = co_await scanner.process(...);
                   {
                       std::lock_guard lock(*chunk_mutex);
                       detailed->merge(scan_output.stats);
                   }
                   co_return;
               });
       }
       co_return;
   });
   
   // All chunks processed; detailed is fully populated

Parallel Execution with when_all/when_any
-------------------------------------------

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

Lazy Sequences and Async Generators
------------------------------------

**Synchronous Generator** - Generate values on-demand without async I/O:

.. code-block:: cpp

   #include <dftracer/utils/core/coro/generator.h>

   // Synchronous generator
   Generator<int> range(int start, int end) {
       for (int i = start; i < end; ++i) {
           co_yield i;
       }
   }

   // Range is lazy -- no computation until iteration
   for (int x : range(0, 100)) {
       process(x);
   }

**Asynchronous Generator** - Generate values on-demand with async I/O. Values are produced via ``co_yield`` and consumed via ``co_await gen.next()``. The consumer suspends until the generator yields or completes.

.. code-block:: cpp

   #include <dftracer/utils/core/coro/async_generator.h>

   // Async generator - streams data from files
   AsyncGenerator<Data> read_all(const std::vector<std::string>& paths) {
       for (const auto& path : paths) {
           Data data = read_file(path);  // Could be async I/O
           co_yield data;
       }
   }

   // Usage in a task
   auto task = make_task([](CoroScope& scope) -> CoroTask<void> {
       auto gen = read_all({"file1.pfw", "file2.pfw"});
       while (auto value = co_await gen.next()) {
           process(*value);
       }
       co_return;
   });

**Real-world example from dftracer_stats** - async line reading:

.. code-block:: cpp

   // Streaming line generator
   AsyncGenerator<std::string> read_lines(const std::string& file_path) {
       // Opens file, decompresses chunks, yields lines on-demand
       auto gen = async_streaming_gz_lines(file_path);
       while (auto line = co_await gen.next()) {
           co_yield *line;
       }
   }

   // Consume in a task
   auto task = make_task([](CoroScope& scope) -> CoroTask<void> {
       auto gen = read_lines("trace.pfw.gz");
       while (auto line = co_await gen.next()) {
           // Process each line as it arrives
           parse_and_update_stats(*line);
       }
       co_return;
   });

Multi-Level Parallelism
-----------------------

For workflows with hierarchical structure (e.g., files → chunks within each file), use nested scopes to parallelize at multiple levels while protecting shared state.

**Pattern:** Outer ``scope.spawn()`` creates parallel per-file tasks, and within each file task, an inner ``scope.scope()`` creates parallel per-chunk tasks. A shared mutex protects accumulated results:

.. code-block:: cpp

    auto task = make_task([](CoroScope& scope) -> coro::CoroTask<void> {
        auto results = std::make_shared<AggregatedResults>();
        auto results_mutex = std::make_shared<std::mutex>();

        // Level 1: Parallel over files
        co_await scope.scope([results, results_mutex](
            CoroScope& file_scope) -> coro::CoroTask<void> {
            for (const auto& file : input_files) {
                file_scope.spawn(
                    [file, results, results_mutex](CoroScope& fctx)
                        -> coro::CoroTask<void> {
                        // Per-file work (e.g., collect metadata, build index)
                        auto metadata = co_await collect_metadata(file);

                        // Get list of chunks in this file
                        auto chunks = get_chunks(metadata);

                        // Level 2: Parallel over chunks in this file
                        co_await fctx.scope([chunks, results,
                                             results_mutex](
                            CoroScope& chunk_scope)
                                -> coro::CoroTask<void> {
                            for (std::size_t i = 0; i < chunks.size();
                                 ++i) {
                                chunk_scope.spawn(
                                    [i, chunks, results, results_mutex](
                                        CoroScope& cctx)
                                        -> coro::CoroTask<void> {
                                        // Process single chunk
                                        auto partial =
                                            co_await process_chunk(chunks[i]);

                                        // Thread-safe merge
                                        {
                                            std::lock_guard lock(*results_mutex);
                                            results->merge(partial);
                                        }
                                        co_return;
                                    });
                            }
                            co_return;
                        });

                        co_return;
                    });
            }
            co_return;
        });

        // All files and chunks complete here; results is fully populated
        finalize(results);
        co_return;
    });

**Real-world example from dftracer_view:** Process files in parallel (Level 1), then for each file, process candidate chunks in parallel (Level 2), accumulating statistics with a mutex:

.. code-block:: cpp

    // Outer scope: one spawn per file
    co_await ctx.scope([&](CoroScope& scope) -> coro::CoroTask<void> {
        for (const auto& file : files) {
            scope.spawn([file](CoroScope& fctx) -> coro::CoroTask<void> {
                // Inner scope: one spawn per candidate chunk in this file
                co_await fctx.scope([](CoroScope& chunk_scope)
                                        -> coro::CoroTask<void> {
                    for (auto chunk_id : candidate_chunks) {
                        chunk_scope.spawn([chunk_id](CoroScope& cctx)
                                             -> coro::CoroTask<void> {
                            auto partial = scan_chunk(chunk_id);
                            {
                                std::lock_guard lock(*chunk_mutex);
                                detailed->merge(partial);
                            }
                            co_return;
                        });
                    }
                    co_return;
                });
                co_return;
            });
        }
        co_return;
    });

**When to use:** Multi-level parallelism is essential when:

- Your data has a natural hierarchy (files → chunks, files → ranges)
- You want to parallelize both levels independently
- Per-file overhead (index building, metadata collection) justifies spawning per-file tasks
- A single flat ``spawn()`` loop would create too many fine-grained tasks

Without nested scopes, you'd flatten to one task per chunk, increasing context-switch overhead and losing logical grouping by file.

Multi-Stage Channel Pipelines
-----------------------------

Chain multiple ``Channel<T>`` instances to build staged processing pipelines. Each stage reads from an upstream channel, processes, and writes to a downstream channel. This achieves streaming backpressure propagation.

**Pattern:** Create one channel per stage, then spawn producer, processing, and consumer tasks that coordinate through channels:

.. code-block:: cpp

    auto task = make_task([](CoroScope& scope) -> coro::CoroTask<void> {
        // Create channels: raw_data -> intermediate -> results
        auto raw_channel =
            coro::make_channel<RawData>(100);  // Buffer 100 items
        auto result_channel =
            coro::make_channel<Result>(50);    // Buffer 50 results

        co_await scope.scope([&](CoroScope& stage_scope)
                                 -> coro::CoroTask<void> {
            // Stage 1: Producers (read files -> raw_channel)
            for (std::size_t i = 0; i < num_producers; ++i) {
                stage_scope.spawn(
                    [i, raw_channel, input_files](CoroScope& pctx)
                        -> coro::CoroTask<void> {
                        auto guard = raw_channel->producer_guard();
                        for (const auto& file : input_files) {
                            RawData data = read_file(file);
                            if (!co_await raw_channel->send(
                                    std::move(data))) {
                                break;
                            }
                        }
                        co_return;
                    });
            }

            // Stage 2: Processors (raw_channel -> result_channel)
            for (std::size_t w = 0; w < num_workers; ++w) {
                stage_scope.spawn(
                    [raw_channel, result_channel](CoroScope& wctx)
                        -> coro::CoroTask<void> {
                        auto guard = result_channel->producer_guard();
                        while (auto data = co_await wctx.receive(raw_channel)) {
                            Result result = process(*data);
                            if (!co_await result_channel->send(
                                    std::move(result))) {
                                break;
                            }
                        }
                        co_return;
                    });
            }

            // Stage 3: Writer (result_channel -> output)
            stage_scope.spawn([result_channel](CoroScope& wctx)
                                  -> coro::CoroTask<void> {
                while (auto result = co_await wctx.receive(result_channel)) {
                    write_output(*result);
                }
                co_return;
            });

            co_return;
        });

        co_return;
    });

**Real-world example from dftracer_aggregator:** Multi-stage streaming pipeline with file producers, chunk processors, and incremental merger:

.. code-block:: cpp

    auto chunk_chan = coro::make_channel<ChunkAggregatorInput>(0);
    auto result_chan = coro::make_channel<ChunkAggregationOutput>(8);

    co_await ctx.scope([&](CoroScope& scope) -> coro::CoroTask<void> {
        chunk_chan->register_producers(input_files.size());

        // Stage 1: File producers
        for (const auto& file_path : input_files) {
            scope.spawn([file_path, chunk_chan](CoroScope& fctx)
                            -> coro::CoroTask<void> {
                auto guard = chunk_chan->adopt_producer();
                auto chunks = extract_chunks(file_path);
                for (auto& chunk : chunks) {
                    co_await chunk_chan->send(std::move(chunk));
                }
                co_return;
            });
        }

        result_chan->register_producers(num_workers);

        // Stage 2: Parallel chunk workers
        for (std::size_t i = 0; i < num_workers; ++i) {
            scope.spawn([chunk_chan, result_chan](CoroScope& wctx)
                            -> coro::CoroTask<void> {
                auto guard = result_chan->adopt_producer();
                while (auto input = co_await wctx.receive(chunk_chan)) {
                    auto output = co_await aggregate_chunk(*input);
                    co_await result_chan->send(std::move(output));
                }
                co_return;
            });
        }

        // Stage 3: Streaming incremental merger
        scope.spawn([result_chan, merger](CoroScope& mctx)
                        -> coro::CoroTask<void> {
            while (auto output = co_await mctx.receive(result_chan)) {
                merger->merge_chunk(std::move(*output));
            }
            co_return;
        });

        co_return;
    });

**Backpressure:** If a processor is slow, its output channel fills up. The next ``send()`` in the upstream stage suspends until space becomes available. This propagates backpressure backward through the pipeline without requiring explicit synchronization.

**Key insights:**

- Each ``producer_guard()`` or ``adopt_producer()`` increments the producer count; when all exit, the channel auto-closes
- Channels buffer ``N`` items; senders block if full, receivers block if empty
- Pipelined stages can run at different speeds, limited only by buffer capacity and not by synchronous task dependencies

Error Handling
--------------

Errors in pipeline tasks propagate to the caller of ``Pipeline::execute()``. Exceptions thrown in task lambdas are captured and re-thrown by the pipeline.

**Exception propagation from tasks:**

.. code-block:: cpp

    auto task = make_task([](CoroScope& scope) -> coro::CoroTask<void> {
        // Any exception thrown here propagates to Pipeline::execute()
        auto result = co_await io::open("missing.txt", O_RDONLY);
        if (result < 0) {
            // Negative result indicates OS error; convert to exception
            throw std::runtime_error(
                "Failed to open file: " +
                std::string(std::strerror(-result)));
        }
        co_return;
    });

    Pipeline pipeline(config);
    pipeline.set_source({task});
    try {
        pipeline.execute();
    } catch (const std::exception& e) {
        std::cerr << "Pipeline failed: " << e.what() << std::endl;
    }

**Error handling in spawned tasks:** When you ``spawn()`` a task that might throw, use ``SpawnFuture<T>`` with a try-catch to handle errors:

.. code-block:: cpp

    auto task = make_task([](CoroScope& scope) -> coro::CoroTask<void> {
        auto future = scope.spawn([](CoroScope& s) -> coro::CoroTask<int> {
            // This might throw
            int result = risky_operation();
            if (result < 0) {
                throw std::runtime_error("Operation failed");
            }
            co_return result;
        });

        try {
            int result = co_await future;
            // Use result
        } catch (const std::exception& e) {
            // Handle error from spawned task
            DFTRACER_UTILS_LOG_WARN("Spawned task failed: %s", e.what());
        }
        co_return;
    });

**Error handling in channels:** Check return values from ``send()`` and ``receive()``. Both return falsy values on closed channels (indicating producer/consumer exit or error):

.. code-block:: cpp

    // Producer
    auto guard = channel->producer_guard();
    for (const auto& item : items) {
        if (!co_await channel->send(std::move(item))) {
            // Channel closed (consumer exited or error)
            DFTRACER_UTILS_LOG_WARN("Channel closed prematurely");
            co_return;
        }
    }

    // Consumer
    while (auto item = co_await channel->receive()) {
        process(*item);
    }
    // Loop exits when channel closes (all producers exited)

Timeouts and Cancellation
--------------------------

Control execution duration and cooperative cancellation using ``PipelineConfig`` watchdog settings and ``CoroScope`` cancellation API.

**Watchdog configuration for timeouts:**

.. code-block:: cpp

    auto config = PipelineConfig()
        .with_name("MyPipeline")
        .with_compute_threads(8)
        .with_watchdog(true)                           // Enable watchdog
        .with_global_timeout(std::chrono::seconds(300)) // 5 min total
        .with_task_timeout(std::chrono::seconds(60));   // 1 min per task

    Pipeline pipeline(config);
    // If execution exceeds global_timeout or a single task exceeds
    // task_timeout, the watchdog logs warnings and may request cancellation

**Cooperative cancellation in tasks:** Tasks check ``scope.is_cancellation_requested()`` and yield cooperatively:

.. code-block:: cpp

    auto task = make_task([](CoroScope& scope) -> coro::CoroTask<void> {
        // Long-running loop that respects cancellation
        while (!scope.is_cancellation_requested()) {
            auto item = co_await channel->receive();
            if (!item) break;  // Channel closed

            process(*item);

            // Yield frequently to allow cancellation checks
            co_await scope.maybe_yield();
        }

        if (scope.is_cancellation_requested()) {
            DFTRACER_UTILS_LOG_INFO("Task cancelled");
        }
        co_return;
    });

**Timing out a race with ``when_any`` + timeout:** Use ``when_any`` with a timeout awaitable to race operations:

.. code-block:: cpp

    #include <dftracer/utils/core/coro/when_any.h>

    auto task = make_task([](CoroScope& scope) -> coro::CoroTask<void> {
        auto cache_future = scope.spawn([](CoroScope& s)
                                           -> coro::CoroTask<Data> {
            co_return co_await read_from_cache();
        });
        auto disk_future = scope.spawn([](CoroScope& s)
                                          -> coro::CoroTask<Data> {
            co_return co_await read_from_disk();
        });

        // Race: cache (fast) vs. disk (slow), with 100ms timeout
        auto timeout_duration = std::chrono::milliseconds(100);
        auto result = co_await when_any({
            std::move(cache_future),
            std::move(disk_future),
            timeout(timeout_duration),
        });

        if (result.index == 0) {
            // Cache won
            use_data(result.result);
        } else if (result.index == 2) {
            // Timeout occurred
            DFTRACER_UTILS_LOG_WARN("Read timeout");
        } else {
            // Disk result
            use_data(result.result);
        }
        co_return;
    });

See :doc:`cpp_api/pipeline` for ``PipelineConfig`` full timeout/watchdog API and :doc:`cpp_api/coro` for ``timeout`` and cancellation details.

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
       [&input_files](CoroScope&, std::size_t idx) -> coro::CoroTask<Metadata> {
           // Each task processes one file
           co_return collect_metadata(input_files[idx]);
       },
       "ProcessFile");

   // Phase 2: Reduce all metadata into chunk manifests
   auto manifests = graph.reduce<std::vector<Manifest>>(
       file_metadata, split_every{input_files.size()},
       [](CoroScope&, std::vector<Metadata> all) -> coro::CoroTask<std::vector<Manifest>> {
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
       [](CoroScope& scope, Data input, std::size_t idx) -> coro::CoroTask<Result> {
           co_return process_shard(input, idx);
       }, "Worker");

   // Fan-in: M -> 1
   auto combined = graph.fan_in<Result>(workers,
       [](CoroScope& scope, std::vector<Result> inputs) -> coro::CoroTask<Result> {
           co_return combine(inputs);
       }, "Combine");

   // Map: 1-to-1 transform
   auto transformed = graph.map<Output>(inputs,
       [](CoroScope& scope, Input in) -> coro::CoroTask<Output> {
           co_return transform(in);
       }, "Transform");

See :doc:`cpp_api/task_graph` for full API.

Migrating from Old Pipeline API (TaskContext/TaskScope)
-------------------------------------------------------

The project has migrated from the old ``TaskContext``/``TaskScope`` API to the new ``CoroScope`` model. Here's how to update existing code:

**Old API (deprecated):**

.. code-block:: cpp

   // Old: Tasks received TaskContext
   auto task = make_task([](TaskContext& ctx) -> CoroTask<void> {
       // Send data
       co_await channel->send_blocking(data);
       
       // Spawn tasks (old way)
       scope.spawn_task([](TaskContext& ctx) -> CoroTask<void> {
           co_return;
       });
       
       co_return;
   });
   
   // Old: PipelineConfig had scheduler threads
   auto config = PipelineConfig()
       .with_scheduler_threads(8)  // Removed!
       .with_name("MyPipeline");

**New API:**

.. code-block:: cpp

   // New: Tasks receive CoroScope
   auto task = make_task([](CoroScope& scope) -> CoroTask<void> {
       // Send data (no _blocking)
       co_await channel->send(std::move(data));
       
       // Spawn tasks (new way)
       scope.spawn([](CoroScope& child) -> CoroTask<void> {
           co_return;
       });
       
       // Or use scope() for structured concurrency
       co_await scope.scope([](CoroScope& child) -> CoroTask<void> {
           child.spawn([](CoroScope& s) -> CoroTask<void> {
               co_return;
           });
           co_return;
       });
       
       co_return;
   });
   
   // New: PipelineConfig uses executor_threads
   auto config = PipelineConfig()
       .with_compute_threads(8)  // Changed from with_scheduler_threads
       .with_name("MyPipeline");

**Key changes:**

+----------------------------------+----------------------------------------------+
| Old                              | New                                          |
+==================================+==============================================+
| ``TaskContext& ctx``             | ``CoroScope& scope``                         |
+----------------------------------+----------------------------------------------+
| ``co_await channel->send_*()``   | ``co_await channel->send()``                 |
+----------------------------------+----------------------------------------------+
| ``co_await channel->receive()``  | ``co_await channel->receive()`` (same)       |
+----------------------------------+----------------------------------------------+
| ``scope.spawn_task(...)``        | ``scope.spawn(...)``                         |
+----------------------------------+----------------------------------------------+
| ``scope.join_all()``             | ``co_await scope.join_all()``                |
+----------------------------------+----------------------------------------------+
| ``with_scheduler_threads(N)``    | ``with_compute_threads(N)``                 |
+----------------------------------+----------------------------------------------+
| ``ctx.spawn_io(...)``            | Various ``io::*`` utilities                  |
+----------------------------------+----------------------------------------------+

**Executor Management:**

- The executor now manages all worker threads internally
- No more explicit ``Scheduler`` creation
- ``PipelineConfig`` configures the executor (threads, timeouts, watchdog)
- ``Pipeline::execute()`` blocks until all work completes

API Reference
-------------

- :doc:`cpp_api/coro` - CoroTask, Channel, Generator, when_all, when_any
- :doc:`cpp_api/task_graph` - TaskGraph, TaskGroup, factory functions
- :doc:`cpp_api/pipeline` - Pipeline, Executor, Task classes
