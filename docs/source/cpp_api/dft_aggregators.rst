DFTracer Aggregation Pipeline
=============================

.. seealso::

   For complete class and member documentation, see the
   :doc:`API Reference <api/utilities/composites/dft/aggregators>`.

Getting Started
---------------

Minimal example using the high-level ``AggregatorUtility``:

.. code-block:: cpp

   #include <dftracer/utils/utilities/composites/dft/aggregators/aggregator_utility.h>

   AggregatorUtility util;
   AggregatorInput input;
   input.directory = "./traces";
   input.config.time_interval_us = 5000000;  // 5-second buckets
   input.config.compute_percentiles = true;

   auto gen = util.process(input);
   while (auto batch = co_await gen.next()) {
       auto arrow = batch->to_arrow();  // 18-column Arrow batch
       // process arrow data...
   }

Event aggregation pipeline for computing statistics over DFTracer trace files.
All classes are in the ``dftracer::utils::utilities::composites::dft::aggregators`` namespace.

The aggregation pipeline processes trace files in parallel chunks, computes
per-key metrics (duration, size, custom fields), and merges results into a
unified output. It supports time bucketing, process hierarchy tracking,
boundary event association, and Perfetto trace output.

.. mermaid::

   graph LR
       subgraph Input
           Files["Trace Files<br/>(.pfw.gz)"]
       end

       subgraph Mapping["Chunk Mapping"]
           CM["FileChunkMapperUtility"]
       end

       subgraph Parallel["Parallel Aggregation"]
           CA1["ChunkAggregatorUtility"]
           CA2["ChunkAggregatorUtility"]
           CAN["ChunkAggregatorUtility"]
       end

       subgraph Merge["Merge & Resolve"]
           EA["EventAggregator"]
           AR["AssociationResolverUtility"]
       end

       subgraph Output
           Perfetto["PerfettoTraceWriterUtility"]
       end

       Files --> CM
       CM --> CA1
       CM --> CA2
       CM --> CAN
       CA1 --> EA
       CA2 --> EA
       CAN --> EA
       EA --> AR
       AR --> Perfetto

Configuration
-------------

AggregationConfig
~~~~~~~~~~~~~~~~~

Main configuration for the aggregation pipeline.

Controls time bucketing, grouping and metric fields, statistical
computation, boundary event tracking, and output format.

.. code-block:: cpp

    AggregationConfig config;
    config.time_interval_us = 1000000;  // 1-second buckets
    config.use_relative_time = true;
    config.compute_statistics = true;
    config.compute_percentiles = true;
    config.percentiles = {0.25, 0.5, 0.75, 0.90, 0.99};

    // Extra JSON fields to add to the grouping key
    config.extra_group_keys = {"args.filename"};

    // Extra numeric JSON fields to accumulate metrics for
    config.custom_metric_fields = {"args.size"};

    // Track boundary events (e.g., epoch boundaries)
    config.boundary_events.push_back({
        .event_name = "epoch_start",
        .value_field = "epoch_id",
        .output_name = "epoch"
    });

Grouping Keys
-------------

AggregationKey
~~~~~~~~~~~~~~

Composite key for grouping events during aggregation.

Events are grouped by category, name, process/thread IDs, host/function hashes,
time bucket, and any extra grouping dimensions specified in the config.

String fields (``cat``, ``name``, ``hhash``, ``fhash``) are stored as interned
``uint32_t`` IDs via a global ``StringIntern`` table (see :doc:`core_infrastructure`),
reducing memory usage and enabling faster hashing. Accessor methods (``.cat()``,
``.name()``, etc.) resolve IDs back to ``string_view``.

Extra key-value pairs use a lazily-allocated ``unique_ptr<vector<pair<uint32_t, uint32_t>>>``
to avoid heap allocation for the common case of no extra keys.

AggregationMap
~~~~~~~~~~~~~~

Type alias for the map from aggregation keys to metrics:

.. code-block:: cpp

   using AggregationMap =
       std::unordered_map<AggregationKey, AggregationMetrics,
                          AggregationKeyHash, AggregationKeyEqual>;

Metrics
-------

AggregationMetrics
~~~~~~~~~~~~~~~~~~

Per-key aggregated metrics using Welford's online algorithm for numerically
stable variance computation and DDSketch for percentile estimation.

Supports incremental updates and merging across chunks.

MetricStats
~~~~~~~~~~~

Single-metric statistics using Welford's online algorithm.

Tracks count, min, max, mean, variance (M2), skewness (M3), kurtosis (M4),
and a DDSketch for percentile estimation. All operations are O(1) per update.

The DDSketch uses a collapsing dense store with 128 fixed bins (``uint16_t``
counters), giving ~256 bytes per sketch. When the bin range exceeds
``MAX_BINS``, the oldest bins are collapsed into bin[0].

System Metrics
--------------

Separate metric types for system-level counters (CPU, memory, ...) that are
aggregated as per-bucket means rather than as event durations. Defined in
``aggregators/system_metrics.h``.

FloatMetricStats
~~~~~~~~~~~~~~~~

Single floating-point metric aggregated with Welford's online mean/variance
plus an optional DDSketch for percentiles.

.. code-block:: cpp

    #include <dftracer/utils/utilities/composites/dft/aggregators/system_metrics.h>

    FloatMetricStats stats(0.01);            // relative sketch accuracy
    stats.update(3.5, /*compute_percentiles=*/true);
    stats.update(4.0, true);
    double sd = stats.get_stddev();          // sample stddev (count-1 divisor)

    FloatMetricStats other(0.01);
    other.update(5.0);
    stats.merge_from(other);                 // combine across chunks

SystemAggregationMetrics
~~~~~~~~~~~~~~~~~~~~~~~~~

A bucket of named ``FloatMetricStats`` plus a timestamp span (``ts``/``te``).
Backs the ``SYSTEM`` aggregation map. ``update_metric`` lazily creates the
per-name entry; ``merge_from`` combines buckets.

.. code-block:: cpp

    SystemAggregationMetrics bucket(0.01);
    bucket.update_metric("cpu_percent", 42.0);
    bucket.update_metric("mem_rss_mb", 1024.0);
    bucket.update_timestamp(event_ts);

Pipeline Stages
---------------

FileChunkMapperUtility
~~~~~~~~~~~~~~~~~~~~~~~

Maps a trace file to parallel chunk work items.

Takes file metadata (from ``MetadataCollectorUtility``) and splits the file
into chunks based on checkpoint boundaries. Each chunk becomes a
``ChunkAggregatorInput`` for parallel processing.

.. code-block:: cpp

    // FileChunkMapperOutput is std::vector<ChunkAggregatorInput>
    coro::CoroTask<FileChunkMapperOutput> process(const FileChunkMapperInput&);

    FileChunkMapperUtility mapper;
    auto input = FileChunkMapperInput::from_metadata(meta)
                     .with_config(config)
                     .with_checkpoint_size(checkpoint_size)
                     .with_target_chunk_size(4);  // MB per chunk
    FileChunkMapperOutput chunks = co_await mapper.process(input);

ChunkAggregatorUtility
~~~~~~~~~~~~~~~~~~~~~~

Per-chunk event aggregation.

Reads events from a byte range within a trace file, applies filters,
computes aggregation keys, and accumulates metrics. Uses bloom filter
predicates for early chunk skipping when available. Multiple instances run
concurrently across chunks.

.. code-block:: cpp

    coro::CoroTask<ChunkAggregationOutput> process(const ChunkAggregatorInput&);

    ChunkAggregatorUtility agg;
    auto input = ChunkAggregatorInput()
                     .with_file_path("trace.pfw.gz")
                     .with_index_path("./traces/.dftindex")
                     .with_byte_range(start, end)
                     .with_config(config)
                     .with_chunk_index(0);
    ChunkAggregationOutput out = co_await agg.process(input);
    // out.aggregations, out.profile_aggregations, out.system_aggregations
    // are AggregationMap instances keyed by AggregationKey.

EventAggregator
~~~~~~~~~~~~~~~

Unified event aggregator (formerly ``EventAggregatorUtility`` and the
internal ``RocksDbAggregator``, now merged into one class). Holds a
``RocksDatabase`` handle and merges per-chunk aggregation results into a
unified output, deduplicating file counts and collecting association
trackers for downstream resolution.

.. code-block:: cpp

    auto db = EventAggregator::open_with_merge_operator(index_path);
    EventAggregator agg(db, config.compute_hash());
    agg.merge_chunk(std::move(chunk_output));   // repeat per chunk
    EventAggregatorOutput result = agg.finalize();
    auto tracker = agg.build_global_tracker();  // merged AssociationTracker

AggregationVisitor
~~~~~~~~~~~~~~~~~~

``DftEventVisitor`` subclass that accumulates ``AggregationMetrics`` per
``AggregationKey`` directly from parsed events during a scan, so the
aggregation pass can share a single parse with bloom and manifest
visitors via ``DftEventDispatcher``. Defined in
``dftracer/utils/utilities/composites/dft/aggregators/aggregation_visitor.h``.

DftEventDispatcher
~~~~~~~~~~~~~~~~~~

Fan-out adapter that implements the ``IndexVisitor`` interface, parses
each line once, and dispatches the parsed ``DftEvent`` to a list of
registered ``DftEventVisitor`` instances (``BloomVisitor``,
``ManifestVisitor``, ``AggregationVisitor``, ...). This collapses
multiple visitor passes into a single read of the input. Defined in
``dftracer/utils/utilities/composites/dft/dft_event_dispatcher.h``.

Association Tracking
--------------------

AssociationTracker
~~~~~~~~~~~~~~~~~~

Tracks process hierarchy (parent-child PIDs) and boundary event intervals
during chunk processing. Each chunk gets its own tracker, and trackers are
merged during the resolution phase.

**Process hierarchy:** Extracts parent PID from metadata events to build
a process tree. Used to annotate aggregated events with their root process.

**Boundary events:** Tracks named intervals (e.g., training epochs) by
matching start/end events. Aggregated events are associated with the
boundary interval that contains their timestamp.

AssociationResolverUtility
~~~~~~~~~~~~~~~~~~~~~~~~~~

Resolves process hierarchy and boundary associations across all chunks.

Merges all per-chunk ``AssociationTracker`` instances, resolves parent PIDs
to root processes, computes trace-wide metadata (duration, boundary ranges),
and annotates aggregated events with their associations.

High-Level Aggregator
---------------------

AggregatorUtility
~~~~~~~~~~~~~~~~~

High-level ``StreamingUtility`` that orchestrates the full aggregation
pipeline: directory scan, index building, metadata collection, chunk
mapping, parallel aggregation, merge, and association resolution.

Yields ``AggregationBatch`` objects that can be converted to Arrow via
``to_arrow()``.

.. code-block:: cpp

   AggregatorUtility util;
   AggregatorInput input;
   input.directory = "./traces";
   input.config.time_interval_us = 1000000;

   auto gen = util.process(input);
   while (auto batch = co_await gen.next()) {
       auto arrow = batch->to_arrow();  // 18-column Arrow batch
       // write to IPC file, send to Python, etc.
   }

Output Utilities
----------------

PerfettoTraceWriterUtility
~~~~~~~~~~~~~~~~~~~~~~~~~~

Writes aggregated results in Perfetto trace format for visualization
in the Perfetto UI (https://ui.perfetto.dev).

Supports three event formats (``PerfettoEventFormat``):

- ``COUNTER`` - Counter track events (default, best for time-series metrics)
- ``ASYNC`` - Async slice events (shows duration spans)
- ``REGULAR`` - Regular slice events

.. code-block:: cpp

    // process() returns coro::CoroTask<bool>; needs a CoroScope context.
    PerfettoTraceWriterInput input;
    input.output_path = "out.pftrace";
    input.aggregator = &aggregator;      // populated EventAggregator
    input.agg_config = &config;
    input.format = PerfettoEventFormat::COUNTER;
    input.compress = true;

    PerfettoTraceWriterUtility writer;
    bool ok = co_await writer.process(input);

Serialization
-------------

RocksDB key/value codecs for the ``AGGREGATION`` column family. Aggregation
keys are packed as a 2-byte shard prefix, a 1-byte ``AggMapType``, and LEB128
varint intern IDs; values are varint-packed metrics with three format tiers
(``METRIC_FMT_COMPACT``, ``METRIC_FMT_FULL``, ``METRIC_FMT_FULL_WITH_SKETCH``).
Defined in ``aggregators/aggregation_serialization.h``.

String fields are interned; the intern dictionary is persisted under the
``0xFFFD`` key prefix, global config under ``0xFFFE``, and per-file
"aggregated" markers under ``0xFFFF``.

.. code-block:: cpp

    #include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>

    std::string key = serialize_agg_key(config_hash, AggMapType::EVENT, agg_key);
    std::string val = serialize_agg_value(metrics);

    DeserializedAggKey dk = deserialize_agg_key(key);
    AggregationMetrics m = deserialize_agg_value(val);

    // Zero-copy views for Arrow export (skip mean/m2/sketch):
    AggKeyView kv;
    AggMetricsView mv;
    if (parse_agg_key_view(key, kv) && parse_agg_value_view(val, mv)) {
        // kv.cat, kv.name, kv.time_bucket; mv.count, mv.dur_total, ...
    }

    // Intern dictionary lifecycle against a live DB:
    load_intern_dictionary(db);
    // ... encode keys ...
    flush_intern_dictionary(db, batch);

Merge Operators
---------------

RocksDB merge operators that combine partial aggregation values written
concurrently by parallel chunk workers, so no read-modify-write is needed on
the hot path.

AggregationMergeOperator
~~~~~~~~~~~~~~~~~~~~~~~~~

Merges event/profile ``AggregationMetrics`` values (defined in
``aggregators/aggregation_merge_operator.h``). Installed via
``EventAggregator::open_with_merge_operator``.

SystemMetricsMergeOperator
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Merges ``SystemAggregationMetrics`` values in the ``SYSTEM_METRICS`` column
family (defined in ``aggregators/system_metrics_merge_operator.h``).

.. code-block:: cpp

    // Both derive from rocksdb::MergeOperator and are set on the CF options:
    cf_options.merge_operator =
        std::make_shared<AggregationMergeOperator>();

Running the Full Pipeline
-------------------------

``run_aggregation`` (in ``aggregators/aggregation_runner.h``) is the one-call
entry point used by the CLI binaries: it scans the log directory, indexes any
files that need it, runs the aggregation visitor pipeline, and optionally emits
a Perfetto JSON / Arrow IPC file. When ``output_file`` is unset it only
populates the ``AGGREGATION`` column family for downstream consumers.

.. code-block:: cpp

    #include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_runner.h>

    AggregationRunInput input;
    input.log_dir = "./traces";
    input.index_dir = "./traces/.dftindex";
    input.agg_config.time_interval_us = 1000000;
    input.output_file = "out.pftrace";              // omit to only fill the CF
    input.event_format = PerfettoEventFormat::COUNTER;

    Result<AggregationRunResult> result = co_await run_aggregation(input);
    if (result) {
        const auto& r = *result;
        // r.index_path, r.total_keys, r.processed_file_count, r.elapsed_ms
    }

