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
           CM["ChunkMapperUtility"]
       end

       subgraph Parallel["Parallel Aggregation"]
           CA1["ChunkAggregatorUtility"]
           CA2["ChunkAggregatorUtility"]
           CAN["ChunkAggregatorUtility"]
       end

       subgraph Merge["Merge & Resolve"]
           EA["EventAggregatorUtility"]
           AR["AssociationResolverUtility"]
       end

       subgraph Output
           Summary["AggregatorSummaryUtility"]
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
       AR --> Summary
       AR --> Perfetto

Configuration
-------------

AggregationConfig
~~~~~~~~~~~~~~~~~

Main configuration for the aggregation pipeline.

Controls time bucketing, event filtering, statistical computation,
boundary event tracking, and output format.

.. code-block:: cpp

    AggregationConfig config;
    config.time_interval_us = 1000000;  // 1-second buckets
    config.use_relative_time = true;
    config.compute_statistics = true;
    config.compute_percentiles = true;
    config.percentiles = {0.25, 0.5, 0.75, 0.90, 0.99};

    // Filter events
    config.include_categories = {"POSIX", "STDIO"};
    config.exclude_names = {"metadata"};

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

Pipeline Stages
---------------

ChunkMapperUtility
~~~~~~~~~~~~~~~~~~

Maps trace files to parallel chunk work items.

Takes file metadata (from ``MetadataCollectorUtility``) and splits each file
into chunks based on checkpoint boundaries. Each chunk becomes a
``ChunkAggregatorInput`` for parallel processing.

ChunkAggregatorUtility
~~~~~~~~~~~~~~~~~~~~~~

Per-chunk event aggregation (parallelizable).

Reads events from a byte range within a trace file, applies filters,
computes aggregation keys, and accumulates metrics. Uses bloom filter
predicates for early chunk skipping when available.

Tagged ``Parallelizable`` — multiple instances run concurrently across chunks.

EventAggregatorUtility
~~~~~~~~~~~~~~~~~~~~~~

Merges per-chunk aggregation results into a unified output.

Combines metrics from all chunks, deduplicates file counts, and
collects association trackers for downstream resolution.

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

AggregatorSummaryUtility
~~~~~~~~~~~~~~~~~~~~~~~~

Outputs a human-readable summary of aggregation results to stdout.

PerfettoTraceWriterUtility
~~~~~~~~~~~~~~~~~~~~~~~~~~

Writes aggregated results in Perfetto trace format for visualization
in the Perfetto UI (https://ui.perfetto.dev).

Supports three event formats:

- ``COUNTER`` — Counter track events (default, best for time-series metrics)
- ``ASYNC`` — Async slice events (shows duration spans)
- ``REGULAR`` — Regular slice events

