Task Graph API
==============

.. seealso::

   For complete class and member documentation, see the
   :doc:`API Reference <api/task_graph>`.


DAG-based task graph builder for constructing parallel computation graphs. All classes are in the ``dftracer::utils::task_graph`` namespace.

.. mermaid::

   graph LR
       subgraph parallel["parallel(N)"]
           T1["Task 0"]
           T2["Task 1"]
           Tn["Task N-1"]
       end

       subgraph reduce["reduce()"]
           R1["Reduce 0-1"]
           R2["Reduce 2-3"]
           RF["Final"]
       end

       T1 --> R1
       T2 --> R1
       Tn --> R2
       R1 --> RF
       R2 --> RF

       subgraph fanout["fan_out(1 to N)"]
           Src["Source"] --> S1["Shard 0"]
           Src --> S2["Shard 1"]
           Src --> Sn["Shard N"]
       end

       subgraph fanin["fan_in(N to 1)"]
           I1["Input 0"] --> Merge["Merge"]
           I2["Input 1"] --> Merge
           In["Input N"] --> Merge
       end

Usage Examples
--------------

Creating a TaskGraph
^^^^^^^^^^^^^^^^^^^^

Build a graph and execute it with Pipeline:

.. code-block:: cpp

    #include <dftracer/utils/core/pipeline/pipeline.h>
    #include <dftracer/utils/core/task_graph/task_graph.h>

    // Create graph builder
    auto graph = TaskGraph::builder({
        .name = "ProcessingGraph",
        .max_concurrency = 128
    });

    // Add operations (see examples below)
    auto results = graph.parallel<OutputType>(
        file_count,
        [](CoroScope& scope, std::size_t idx) -> coro::CoroTask<OutputType> {
            co_return process_item(idx);
        },
        {.name = "ProcessItem"}
    );

    // Execute with Pipeline
    Pipeline pipeline(pipeline_config);
    pipeline.set_source(graph.first_task());
    pipeline.execute();

parallel() - Process N Independent Tasks
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Spawn N parallel tasks that each produce a result. Each task receives its index (0 to count-1):

.. code-block:: cpp

    auto graph = TaskGraph::builder({.name = "Parallel"});
    
    // Process 8 files in parallel
    auto file_results = graph.parallel<FileMetadata>(
        8,
        [](CoroScope& scope, std::size_t idx) -> coro::CoroTask<FileMetadata> {
            std::string filename = "file_" + std::to_string(idx) + ".pfw.gz";
            // Load file, parse header, return metadata
            auto metadata = load_file_metadata(filename);
            co_return metadata;
        },
        {.name = "LoadMetadata"}
    );

With ``max_concurrency`` set, a sliding window limits in-flight tasks:

.. code-block:: cpp

    // Allow only 4 tasks in-flight at once
    auto results = graph.parallel<int>(
        100,
        [](CoroScope& scope, std::size_t idx) -> coro::CoroTask<int> {
            co_return expensive_computation(idx);
        },
        {.name = "Compute", .max_concurrency = 4}
    );

reduce() - Combine N Results Into One
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Reduce parallel results using a tree reduction (O(log N) depth). Combine every 2 results:

.. code-block:: cpp

    auto graph = TaskGraph::builder({.name = "Reduce"});
    
    // Create 8 parallel tasks
    auto file_results = graph.parallel<Metadata>(/*...*/);
    
    // Reduce: combine pairs, then combine those results
    auto combined = graph.reduce<CombinedMetadata>(
        file_results,
        split_every{2},  // Combine 2 results at each level
        [](CoroScope& scope, std::vector<Metadata> batch)
            -> coro::CoroTask<CombinedMetadata> {
            // Merge batch into single result
            CombinedMetadata merged;
            for (const auto& meta : batch) {
                merged.merge(meta);
            }
            co_return merged;
        },
        {.name = "MergeMetadata"}
    );

fan_out() - Split One Input Into N Outputs
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Fan-out distributes a single input to N downstream tasks by index:

.. code-block:: cpp

    auto graph = TaskGraph::builder({.name = "FanOut"});
    
    // Source produces one large dataset
    auto source = graph.source<Dataset>(
        [](CoroScope& scope) -> coro::CoroTask<Dataset> {
            co_return load_full_dataset();
        },
        {.name = "LoadDataset"}
    );
    
    // Fan-out: split into 4 shards
    auto shards = graph.fan_out<DatasetShard>(
        source,
        num_outputs{4},
        [](CoroScope& scope, const Dataset& input, std::size_t shard_idx)
            -> coro::CoroTask<DatasetShard> {
            // Extract shard_idx-th quarter of dataset
            auto shard = input.partition(shard_idx, 4);
            co_return shard;
        },
        {.name = "Shard"}
    );

fan_in() - Merge N Inputs Into One
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Combine multiple task outputs into a single result:

.. code-block:: cpp

    // Fan-in: combine all shards back together
    auto merged = graph.fan_in<Dataset>(
        shards,
        [](CoroScope& scope, std::vector<DatasetShard> inputs)
            -> coro::CoroTask<Dataset> {
            // Combine all shards back to full dataset
            Dataset full;
            for (const auto& shard : inputs) {
                full.append(shard);
            }
            co_return full;
        },
        {.name = "Merge"}
    );

Fan-in with grouping (combine every N inputs):

.. code-block:: cpp

    // Combine every 2 shards (reduce M -> ceil(M/2) outputs)
    auto grouped = graph.fan_in<DatasetShard>(
        shards,
        split_every{2},
        [](CoroScope& scope, std::vector<DatasetShard> inputs)
            -> coro::CoroTask<DatasetShard> {
            DatasetShard result = inputs[0];
            for (std::size_t i = 1; i < inputs.size(); ++i) {
                result.merge(inputs[i]);
            }
            co_return result;
        },
        {.name = "GroupedMerge"}
    );

map() - Transform Each Item 1:1
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Transform each task's output (streaming 1:1 mapping):

.. code-block:: cpp

    auto graph = TaskGraph::builder({.name = "Map"});
    
    // Create parallel tasks
    auto file_results = graph.parallel<RawData>(/*...*/);
    
    // Map: transform each result
    auto normalized = graph.map<NormalizedData>(
        file_results,
        [](CoroScope& scope, const RawData& raw) -> coro::CoroTask<NormalizedData> {
            // Process raw data
            auto norm = normalize(raw);
            co_return norm;
        },
        {.name = "Normalize", .max_concurrency = 8}
    );

fold() - Accumulate With Initial Value
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Reduce with an initial accumulator value using a binary operation:

.. code-block:: cpp

    auto graph = TaskGraph::builder({.name = "Fold"});
    
    // Create parallel task outputs
    auto counts = graph.parallel<int>(/*...*/);
    
    // Fold: sum all counts with initial value 0
    auto total = graph.fold<int>(
        counts,
        0,  // Initial accumulator
        split_every{2},
        [](int acc, int next) -> int { return acc + next; },
        {.name = "Sum"}
    );

aggregate() - Map-Reduce Pattern
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Apply a mapper to each item, then reduce the results:

.. code-block:: cpp

    auto graph = TaskGraph::builder({.name = "Aggregate"});
    
    // Create parallel tasks
    auto events = graph.parallel<Event>(/*...*/);
    
    // Aggregate: map events to statistics, then combine
    auto stats = graph.aggregate<Statistics, EventStats>(
        events,
        // Mapper: Event -> EventStats
        [](CoroScope& scope, const Event& evt) -> coro::CoroTask<EventStats> {
            auto stat = compute_event_stats(evt);
            co_return stat;
        },
        split_every{4},
        // Reducer: combine EventStats
        [](CoroScope& scope, std::vector<EventStats> batch)
            -> coro::CoroTask<Statistics> {
            Statistics merged;
            for (const auto& stat : batch) {
                merged.combine(stat);
            }
            co_return merged;
        },
        {.name = "AggregateEvents"}
    );

partition() - Split Data Into Groups
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Partition static data into N contiguous chunks (no coroutine tasks):

.. code-block:: cpp

    auto graph = TaskGraph::builder({.name = "Partition"});
    
    std::vector<int> data = {0, 1, 2, 3, 4, 5, 6, 7};
    
    // Split into 4 partitions
    auto partitions = graph.partition<int>(
        data,
        num_partitions{4},
        {.name = "Split"}
    );
    
    // partitions contains 4 tasks, each producing std::vector<int>
    // Task 0: [0, 1], Task 1: [2, 3], Task 2: [4, 5], Task 3: [6, 7]

concat_partitions() - Merge Partitions Back Together
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Recombine partitioned vectors into a single vector:

.. code-block:: cpp

    auto graph = TaskGraph::builder({.name = "Concat"});
    
    // Create partitions (see above)
    auto partitions = graph.partition<int>(data, num_partitions{4});
    
    // Concatenate back into single vector
    auto full = graph.concat_partitions<int>(
        partitions,
        split_every{2},
        {.name = "Join"}
    );
    // full.task() produces std::vector<int> with all elements

Complete Real-World Example: Split & Reduce
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

This example shows a 3-phase pipeline similar to ``dftracer_split``:

.. code-block:: cpp

    #include <dftracer/utils/core/pipeline/pipeline.h>
    #include <dftracer/utils/core/task_graph/task_graph.h>

    struct FileMetadata { /* file info */ };
    struct ChunkManifest { /* chunk boundaries */ };
    struct ExtractResult { /* chunk data */ };

    int main() {
        // Configure pipeline
        PipelineConfig config;
        config.executor_threads = 8;
        
        // Build task graph
        auto graph = TaskGraph::builder({
            .name = "SplitPipeline",
            .max_concurrency = 0  // Unlimited
        });

        // Phase 1: Load metadata for all files in parallel
        auto file_metadata = graph.parallel<FileMetadata>(
            10,  // 10 files
            [](CoroScope& scope, std::size_t idx) -> coro::CoroTask<FileMetadata> {
                std::string filename = "trace_" + std::to_string(idx) + ".pfw.gz";
                auto meta = load_file_header(filename);
                co_return meta;
            },
            {.name = "LoadMetadata"}
        );

        // Phase 2: Reduce metadata to build global manifests
        auto manifests = graph.reduce<std::vector<ChunkManifest>>(
            file_metadata,
            split_every{2},
            [](CoroScope& scope, std::vector<FileMetadata> batch)
                -> coro::CoroTask<std::vector<ChunkManifest>> {
                std::vector<ChunkManifest> result;
                for (const auto& meta : batch) {
                    auto chunks = meta.create_chunks(1 << 20);  // 1 MB chunks
                    result.insert(result.end(), chunks.begin(), chunks.end());
                }
                co_return result;
            },
            {.name = "CreateManifests"}
        );

        // Phase 3: Extract chunks in parallel
        auto task_extract = make_task(
            [](CoroScope& scope, std::vector<ChunkManifest> manifests)
                -> coro::CoroTask<std::vector<ExtractResult>> {
                std::vector<ExtractResult> results;
                for (const auto& manifest : manifests) {
                    auto chunk = extract_chunk(manifest);
                    results.push_back(chunk);
                }
                co_return results;
            },
            "ExtractChunks"
        );
        task_extract->depends_on(manifests.task());
        graph.add(task_extract);

        // Execute graph
        Pipeline pipeline(config);
        pipeline.set_source(graph.first_task());
        pipeline.execute();

        std::cout << "Split complete!" << std::endl;
        return 0;
    }

Configuration
-------------

TaskGraphConfig
^^^^^^^^^^^^^^^

Global configuration for task graph execution.

Controls graph-level settings including:

- ``name``: Identifier for the task graph
- ``max_concurrency``: Maximum number of parallel tasks in-flight (0 = unlimited)

Individual task operations (``parallel()``, ``fan_out()``, ``map()``) have their own config structs
that can override the graph-level ``max_concurrency`` when set to a non-zero value.

TaskGraphSourceConfig
^^^^^^^^^^^^^^^^^^^^^

Configuration for ``source()`` operations.

- ``name``: Identifier for the source task (default: ``"Source"``)

TaskGraphParallelConfig
^^^^^^^^^^^^^^^^^^^^^^^

Configuration for ``parallel()`` task execution.

- ``name``: Identifier for the parallel group (default: ``"Parallel"``)
- ``max_concurrency``: Override graph-level concurrency for this group (0 = use graph default)

TaskGraphFanOutConfig
^^^^^^^^^^^^^^^^^^^^^

Configuration for ``fan_out()`` operations.

- ``name``: Identifier for the fan-out group (default: ``"FanOut"``)
- ``max_concurrency``: Override graph-level concurrency for this group (0 = use graph default)

TaskGraphFanInConfig
^^^^^^^^^^^^^^^^^^^^

Configuration for ``fan_in()`` operations.

- ``name``: Identifier for the fan-in task (default: ``"FanIn"``)

TaskGraphMapConfig
^^^^^^^^^^^^^^^^^^

Configuration for ``map()`` operations.

- ``name``: Identifier for the map group (default: ``"Map"``)
- ``max_concurrency``: Override graph-level concurrency for this group (0 = use graph default)

TaskGraphReduceConfig
^^^^^^^^^^^^^^^^^^^^^

Configuration for ``reduce()`` operations.

- ``name``: Identifier for the reduce task (default: ``"Reduce"``)

TaskGraphFoldConfig
^^^^^^^^^^^^^^^^^^^

Configuration for ``fold()`` operations.

- ``name``: Identifier for the fold task (default: ``"Fold"``)

TaskGraphAggregateConfig
^^^^^^^^^^^^^^^^^^^^^^^^

Configuration for ``aggregate()`` operations.

- ``name``: Identifier for the aggregate task (default: ``"Aggregate"``)

TaskGraphPartitionConfig
^^^^^^^^^^^^^^^^^^^^^^^^

Configuration for ``partition()`` operations.

- ``name``: Identifier for the partition task (default: ``"Partition"``)

TaskGraphConcatConfig
^^^^^^^^^^^^^^^^^^^^^

Configuration for ``concat_partitions()`` operations.

- ``name``: Identifier for the concat task (default: ``"Concat"``)

.. tip::

   All config structs share a common ``name`` field for identifying the operation in
   logs and diagnostics. Operations that support parallelism (``parallel``, ``fan_out``,
   ``map``) additionally expose ``max_concurrency`` to override the graph-level setting.

   For complete struct definitions including default values, see the
   :doc:`API Reference <api/task_graph>`.

