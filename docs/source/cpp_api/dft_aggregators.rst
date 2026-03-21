DFTracer Aggregation Pipeline
=============================

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

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::aggregators::AggregationConfig
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::aggregators::BoundaryEventConfig
   :project: dftracer-utils
   :members:
   :undoc-members:

Grouping Keys
-------------

AggregationKey
~~~~~~~~~~~~~~

Composite key for grouping events during aggregation.

Events are grouped by category, name, process/thread IDs, host/function hashes,
time bucket, and any extra grouping dimensions specified in the config.

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::aggregators::AggregationKey
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::aggregators::AggregationKeyHash
   :project: dftracer-utils
   :members:
   :undoc-members:

Metrics
-------

AggregationMetrics
~~~~~~~~~~~~~~~~~~

Per-key aggregated metrics using Welford's online algorithm for numerically
stable variance computation and DDSketch for percentile estimation.

Supports incremental updates and merging across chunks.

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::aggregators::AggregationMetrics
   :project: dftracer-utils
   :members:
   :undoc-members:

MetricStats
~~~~~~~~~~~

Single-metric statistics using Welford's online algorithm.

Tracks count, min, max, mean, variance (M2), skewness (M3), kurtosis (M4),
and a DDSketch for percentile estimation. All operations are O(1) per update.

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::aggregators::MetricStats
   :project: dftracer-utils
   :members:
   :undoc-members:

Pipeline Stages
---------------

ChunkMapperUtility
~~~~~~~~~~~~~~~~~~

Maps trace files to parallel chunk work items.

Takes file metadata (from ``MetadataCollectorUtility``) and splits each file
into chunks based on checkpoint boundaries. Each chunk becomes a
``ChunkAggregatorInput`` for parallel processing.

**Single-file variant:**

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::aggregators::FileChunkMapperInput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::composites::dft::aggregators::FileChunkMapperUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

**Multi-file variant:**

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::aggregators::ChunkMapperInput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::composites::dft::aggregators::ChunkMapperUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

ChunkAggregatorUtility
~~~~~~~~~~~~~~~~~~~~~~

Per-chunk event aggregation (parallelizable).

Reads events from a byte range within a trace file, applies filters,
computes aggregation keys, and accumulates metrics. Uses bloom filter
predicates for early chunk skipping when available.

Tagged ``Parallelizable`` — multiple instances run concurrently across chunks.

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::aggregators::ChunkAggregatorInput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::aggregators::ChunkAggregationOutput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::composites::dft::aggregators::ChunkAggregatorUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

EventAggregatorUtility
~~~~~~~~~~~~~~~~~~~~~~

Merges per-chunk aggregation results into a unified output.

Combines metrics from all chunks, deduplicates file counts, and
collects association trackers for downstream resolution.

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::aggregators::EventAggregatorUtilityInput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::aggregators::EventAggregatorUtilityOutput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::composites::dft::aggregators::EventAggregatorUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

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

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::aggregators::BoundaryInterval
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::composites::dft::aggregators::AssociationTracker
   :project: dftracer-utils
   :members:
   :undoc-members:

AssociationResolverUtility
~~~~~~~~~~~~~~~~~~~~~~~~~~

Resolves process hierarchy and boundary associations across all chunks.

Merges all per-chunk ``AssociationTracker`` instances, resolves parent PIDs
to root processes, computes trace-wide metadata (duration, boundary ranges),
and annotates aggregated events with their associations.

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::aggregators::AssociationResolverInput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::aggregators::AssociationResolverOutput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::composites::dft::aggregators::AssociationResolverUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

High-Level Aggregator
---------------------

AggregatorUtility
~~~~~~~~~~~~~~~~~

High-level ``StreamingUtility`` that orchestrates the full aggregation
pipeline: directory scan, index building, metadata collection, chunk
mapping, parallel aggregation, merge, and association resolution.

Yields ``AggregationBatch`` objects that can be converted to Arrow via
``to_arrow()``.

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::aggregators::AggregatorInput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::aggregators::AggregationBatch
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::composites::dft::aggregators::AggregatorUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

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

.. doxygenclass:: dftracer::utils::utilities::composites::dft::aggregators::AggregatorSummaryUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

PerfettoTraceWriterUtility
~~~~~~~~~~~~~~~~~~~~~~~~~~

Writes aggregated results in Perfetto trace format for visualization
in the Perfetto UI (https://ui.perfetto.dev).

Supports three event formats:

- ``COUNTER`` — Counter track events (default, best for time-series metrics)
- ``ASYNC`` — Async slice events (shows duration spans)
- ``REGULAR`` — Regular slice events

.. doxygenenum:: dftracer::utils::utilities::composites::dft::aggregators::PerfettoEventFormat
   :project: dftracer-utils

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::aggregators::PerfettoTraceWriterInput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::composites::dft::aggregators::PerfettoTraceWriterUtility
   :project: dftracer-utils
   :members:
   :undoc-members:
