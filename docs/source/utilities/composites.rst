Composites
===================

High-level utilities that combine multiple operations into complete workflows.

.. code-block:: cpp

   #include <dftracer/utils/utilities/composites/composites.h>

   using namespace dftracer::utils::utilities::composites;

BatchProcessorUtility
---------------------

Processes multiple items in parallel with optional result sorting.

.. code-block:: cpp

   template <typename Input, typename Output>
   class BatchProcessorUtility;

**Example:**

.. code-block:: cpp

   // Create workflow utility
   auto workflow = std::make_shared<MyItemProcessor>();

   // Wrap in batch processor
   auto batch = std::make_shared<BatchProcessorUtility<ItemInput, ItemOutput>>(workflow);

   // Optional: sort results by index
   batch->with_comparator([](const ItemOutput& a, const ItemOutput& b) {
       return a.index < b.index;
   });

   // Process items
   std::vector<ItemInput> inputs = /* ... */;
   std::vector<ItemOutput> outputs = batch->process(inputs);

DirectoryFileProcessorUtility
-----------------------------

Scans directory and processes each matching file in parallel.

.. code-block:: cpp

   template <typename FileOutput>
   class DirectoryFileProcessorUtility;

**Input:**

.. code-block:: cpp

   struct DirectoryProcessInput {
       fs::path directory;
       std::vector<std::string> extensions;  // e.g., {".pfw", ".gz"}
       bool recursive = false;

       static DirectoryProcessInput from_directory(const fs::path& dir);
       DirectoryProcessInput& with_extensions(std::vector<std::string> exts);
       DirectoryProcessInput& with_recursive(bool value);
   };

**Output:**

.. code-block:: cpp

   template <typename T>
   struct BatchFileProcessOutput {
       std::vector<T> results;
       std::size_t total_files;
       std::size_t successful_files;
   };

LineBatchProcessorUtility
-------------------------

Streaming line processing with optional filtering.

.. code-block:: cpp

   template <typename LineOutput>
   class LineBatchProcessorUtility;

Processes lines lazily and returns vector of results. Line processor returns ``std::optional<LineOutput>`` to filter lines.

**Example:**

.. code-block:: cpp

   auto processor = std::make_shared<LineBatchProcessorUtility<ParsedEvent>>(
       [](const Line& line) -> std::optional<ParsedEvent> {
           if (line.content.empty()) return std::nullopt;
           return parse_event(line);
       }
   );

   LineReadInput input{
       "/path/to/file.pfw.gz",
       "/path/to/file.pfw.gz.idx",
       0,     // start_line
       1000   // end_line
   };

   std::vector<ParsedEvent> events = processor->process(input);

FileCompressorUtility
---------------------

Compresses files with streaming.

**Input:**

.. code-block:: cpp

   struct FileCompressionUtilityInput {
       fs::path input_path;
       fs::path output_path;
       int compression_level = 6;

       static FileCompressionUtilityInput from_file(const fs::path& path);
       FileCompressionUtilityInput& with_output(const fs::path& path);
       FileCompressionUtilityInput& with_level(int level);
   };

**Output:**

.. code-block:: cpp

   struct FileCompressionUtilityOutput {
       bool success;
       std::size_t original_size;
       std::size_t compressed_size;
       double compression_ratio;
   };

FileDecompressorUtility
-----------------------

Decompresses files with streaming.

**Input:**

.. code-block:: cpp

   struct FileDecompressionUtilityInput {
       fs::path input_path;
       fs::path output_path;

       static FileDecompressionUtilityInput from_file(const fs::path& path);
       FileDecompressionUtilityInput& with_output(const fs::path& path);
   };

IndexedFileReaderUtility
------------------------

Creates readers for indexed compressed files, handling index creation.

.. code-block:: cpp

    struct IndexedReadInput {
        fs::path file_path;
        fs::path index_path;
        std::size_t checkpoint_size = DEFAULT_CHECKPOINT_SIZE;
        bool force_rebuild = false;

        static IndexedReadInput from_file(const fs::path& path);
        IndexedReadInput& with_index(const fs::path& path);
        IndexedReadInput& with_checkpoint_size(std::size_t size);
        IndexedReadInput& with_force_rebuild(bool value);
    };

**Example:**

.. code-block:: cpp

    IndexedFileReaderUtility utility;

    auto input = IndexedReadInput::from_file("/data/trace.pfw.gz")
        .with_index("/data/trace.pfw.gz.idx")
        .with_force_rebuild(false);

    std::shared_ptr<Reader> reader = utility.process(input);

DFTracer-Specific Composites
============================

These composites are specialized for analyzing DFTracer HPC trace files. They provide distributed statistics collection, bloom filter-based chunk skipping, view queries with predicate filtering, and aggregation across multiple files.

.. code-block:: cpp

    #include <dftracer/utils/utilities/composites/dft/statistics/detailed_statistics.h>
    #include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter_cache.h>
    #include <dftracer/utils/utilities/composites/dft/views/predicate_filter.h>

Statistics
----------

DDSketch and Log2Histogram
~~~~~~~~~~~~~~~~~~~~~~~~~~

Two probabilistic data structures for efficient, order-independent statistics:

**DDSketch** — Percentile estimation with bounded relative error:

.. code-block:: cpp

    #include <dftracer/utils/utilities/common/statistics/ddsketch.h>

    using dftracer::utils::utilities::common::statistics::DDSketch;

    // Create sketch with 1% relative error bound
    DDSketch sketch(0.01);

    // Add event durations
    for (auto duration_us : event_durations) {
        sketch.add(duration_us);
    }

    // Query percentiles
    double p50 = sketch.quantile(0.5);      // median
    double p99 = sketch.quantile(0.99);     // 99th percentile
    double p999 = sketch.quantile(0.999);   // 99.9th percentile

    // Merge sketches from parallel workers (order-independent)
    DDSketch merged(0.01);
    merged.merge(sketch_from_worker_1);
    merged.merge(sketch_from_worker_2);
    // merged now contains combined percentiles

    // Access raw statistics
    std::uint64_t count = sketch.count();
    double min_val = sketch.min();
    double max_val = sketch.max();
    std::size_t memory_bytes = sketch.memory_usage();

**Log2Histogram** — Fixed 65-bin logarithmic histogram for size distributions:

.. code-block:: cpp

    #include <dftracer/utils/utilities/common/statistics/log2_histogram.h>

    using dftracer::utils::utilities::common::statistics::Log2Histogram;

    // Create histogram with bins for powers of 2
    Log2Histogram histogram;

    // Add event sizes
    for (auto size_bytes : event_sizes) {
        histogram.add(size_bytes);
    }

    // Bin layout:
    // Bin 0: value == 0
    // Bin k (1 <= k <= 64): values in [2^(k-1), 2^k)
    // Covers full uint64_t range (0 to 2^63)

    // Query approximate percentile
    double p95 = histogram.approx_percentile(0.95);

    // Merge histograms (commutative)
    Log2Histogram merged;
    merged.merge(histogram);

    // Human-readable output
    std::string ascii_chart = histogram.render_ascii(80, "microseconds");
    std::string block_chart = histogram.render_blocks(80, "bytes");
    std::cout << ascii_chart << std::endl;

    // JSON serialization for storage
    std::string json_str = histogram.to_json();
    Log2Histogram restored = Log2Histogram::from_json(json_str);

Chunk Statistics
~~~~~~~~~~~~~~~~

Per-chunk aggregation of event counts, timestamps, and duration distributions:

.. code-block:: cpp

    #include <dftracer/utils/utilities/composites/dft/indexing/chunk_statistics.h>

    using dftracer::utils::utilities::composites::dft::indexing::ChunkStatistics;

    // Initialize per-chunk tracker
    ChunkStatistics chunk_stats;

    // Update as events are scanned
    for (auto& event : events_in_chunk) {
        chunk_stats.update_from_event(
            event.name,               // e.g., "read", "write"
            event.category,           // e.g., "POSIX", "DLIO"
            event.pid, event.tid,
            event.timestamp_us,
            event.duration_us
        );
    }

    // Access aggregated data
    std::uint64_t total = chunk_stats.total_events;
    auto& cat_counts = chunk_stats.category_counts;    // name -> count
    auto& op_counts = chunk_stats.name_counts;         // operation -> count
    auto& tid_counts = chunk_stats.pid_tid_counts;     // "pid:tid" -> count

    // Duration statistics (Welford's online algorithm + sketches)
    double mean_duration = chunk_stats.duration_mean();
    double variance = chunk_stats.duration_variance();
    double p50 = chunk_stats.duration_sketch.quantile(0.5);

    // Per-operation duration distributions
    auto& name_sketches = chunk_stats.name_duration_sketches;
    auto& name_histograms = chunk_stats.name_duration_histograms;

    // Merge statistics from multiple workers
    ChunkStatistics merged;
    merged.merge_from(chunk_stats);

    // JSON serialization for SQLite storage
    std::string cat_json = chunk_stats.category_counts_json();
    std::string hist_json = chunk_stats.name_duration_histograms_json();

Bloom Filter Indexing
---------------------

BloomFilterCache
~~~~~~~~~~~~~~~~

Thread-safe bounded cache for deserialized bloom filters used during chunk skipping:

.. code-block:: cpp

    #include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter_cache.h>

    using dftracer::utils::utilities::composites::dft::indexing::BloomFilterCache;

    // Create cache with max 10,000 entries (auto-clears when full)
    BloomFilterCache cache(BloomFilterCache::DEFAULT_MAX_ENTRIES);

    // Typical usage during index query: check if event might be in chunk
    const std::string idx_path = "/data/trace.pfw.gz.idx";
    const std::string dimension = "name";  // which filter (by operation name)
    std::uint64_t checkpoint_idx = 5;     // chunk number

    // Look up cached bloom filter
    auto cached = cache.get(idx_path, dimension, checkpoint_idx);
    if (cached) {
        // Filter was in cache
        auto& filter = cached.value();
        if (filter.possibly_contains("read")) {
            // Event might be in this chunk - need to scan
        } else {
            // Event definitely NOT in this chunk - safe to skip
        }
    } else {
        // Cache miss - load filter from .idx file, add to cache
        auto filter = load_bloom_from_idx(idx_path, dimension, checkpoint_idx);
        cache.put(idx_path, dimension, checkpoint_idx, filter);
    }

    // For file-level bloom filters
    std::uint64_t FILE_LEVEL = BloomFilterCache::FILE_LEVEL_SENTINEL;  // UINT64_MAX
    cache.put(idx_path, dimension, FILE_LEVEL, file_level_filter);

    // Cache statistics
    std::size_t cache_size = cache.size();

View Queries with Predicates
-----------------------------

PredicateFilter
~~~~~~~~~~~~~~~

Build and evaluate event matching predicates for view queries:

.. code-block:: cpp

    #include <dftracer/utils/utilities/composites/dft/views/predicate_filter.h>
    #include <dftracer/utils/utilities/composites/dft/views/view_definition.h>

    using dftracer::utils::utilities::composites::dft::views::PredicateFilter;
    using dftracer::utils::utilities::composites::dft::views::build_predicate_filter;
    using dftracer::utils::utilities::composites::dft::views::matches_predicate;
    using dftracer::utils::utilities::composites::dft::views::matches_any_predicate;

    // Define view query predicates
    ViewPredicate predicate;
    predicate.dimensions["name"] = {"read", "write"};     // event name filter
    predicate.dimensions["cat"] = {"POSIX", "DLIO"};      // category filter
    predicate.time_range = {1000.0, 5000.0};              // microseconds
    predicate.duration_bounds = {10.0, 100.0};            // microseconds

    // Compile to efficient filter representation
    PredicateFilter filter = build_predicate_filter(predicate);

    // Check if a parsed event matches
    auto& json_event = parsed_json_value;
    if (matches_predicate(json_event, filter)) {
        // Include this event in view results
        results.push_back(json_event);
    }

    // Check multiple predicates (OR semantics)
    std::vector<PredicateFilter> filters = {
        build_predicate_filter(read_predicate),
        build_predicate_filter(write_predicate)
    };

    if (matches_any_predicate(json_event, filters)) {
        // Event matches at least one predicate
        results.push_back(json_event);
    }

Aggregation Patterns
--------------------

DistributionStats and DetailedStatistics
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Reusable building blocks for multi-worker aggregation:

.. code-block:: cpp

    #include <dftracer/utils/utilities/composites/dft/statistics/detailed_statistics.h>

    using dftracer::utils::utilities::composites::dft::statistics::DistributionStats;
    using dftracer::utils::utilities::composites::dft::statistics::DetailedStatistics;
    using dftracer::utils::utilities::composites::dft::statistics::IOEventMetrics;

    // Per-worker collection (one worker per file/chunk batch)
    DistributionStats duration_stats;
    for (auto& event : events) {
        duration_stats.update(event.duration_us);
    }

    // DistributionStats combines three representations
    // - histogram: 65 fixed bins
    // - sketch: percentile estimation (0.01 relative error)
    // - sum: for mean/variance computation
    double mean = duration_stats.mean();
    double stddev = duration_stats.stddev();
    std::uint64_t count = duration_stats.count();

    // Per-group aggregation (group by operation name, category, etc.)
    DetailedStatistics stats;
    stats.duration.update(event.duration_us);  // global duration

    // Group by operation name
    std::string key = event.name;
    stats.grouped_duration[key].update(event.duration_us);

    // I/O event metrics (if event has I/O fields)
    auto& io_metrics = stats.grouped_io[key];
    io_metrics.duration.update(event.duration_us);
    io_metrics.size.update(event.size_bytes);
    io_metrics.bandwidth.update(bytes_per_second);
    io_metrics.offset.update(file_offset);

    // Merge results from all workers (order-independent)
    DetailedStatistics merged;
    for (auto& worker_stats : worker_results) {
        merged.merge(worker_stats);
    }

    // Export to JSON for further analysis
    std::string json_output = merged.to_json();

Real-World Example: Statistics Pipeline
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Complete example of gathering statistics from a DFTracer trace file:

.. code-block:: cpp

    #include <dftracer/utils/utilities/composites/dft/statistics/statistics_aggregator_utility.h>
    #include <dftracer/utils/utilities/composites/dft/indexing/bloom_filter_cache.h>

    using dftracer::utils::utilities::composites::dft::statistics::
        StatisticsAggregatorUtility;
    using dftracer::utils::utilities::composites::dft::statistics::
        StatisticsAggregatorInput;
    using dftracer::utils::utilities::composites::dft::indexing::BloomFilterCache;

    // Create aggregator utility
    auto aggregator = std::make_shared<StatisticsAggregatorUtility>();

    // Prepare input (uses bloom index for chunk skipping)
    StatisticsAggregatorInput input{
        .file_path = "/data/trace.pfw.gz",
        .idx_path = "/data/trace.pfw.gz.idx",
        .index_dir = "/data/.indexes"
    };

    // Run aggregation (coroutine-based, parallelizable)
    auto stats = co_await aggregator->process(input);

    // Results contain merged statistics across all chunks
    // - Overall duration percentiles (p50, p99, p999)
    // - Per-operation counts and distributions
    // - Category and thread breakdowns
    std::cout << "Total events: " << stats.merged.total_events << std::endl;
    std::cout << "Duration p99: " << stats.merged.duration_sketch.quantile(0.99)
              << " us" << std::endl;

DFT Event Pipeline
------------------

DftEventDispatcher
~~~~~~~~~~~~~~~~~~

Adapter that turns a list of ``DftEventVisitor`` instances into a single
``IndexVisitor`` consumable by ``IndexBuilderUtility``. Owns a per-instance
``JsonParser`` and parses each decompressed line once before fanning out to
the configured visitors (``BloomVisitor``, ``ManifestVisitor``,
``AggregationVisitor``, ...). Supports a ``force_serial`` mode for
deterministic-order replays.

.. code-block:: cpp

    #include <dftracer/utils/utilities/composites/dft/dft_event_dispatcher.h>

    std::vector<std::unique_ptr<DftEventVisitor>> visitors;
    visitors.push_back(std::make_unique<BloomVisitor>(...));
    visitors.push_back(std::make_unique<ManifestVisitor>(...));
    DftEventDispatcher dispatcher(std::move(visitors));

AggregationVisitor
~~~~~~~~~~~~~~~~~~

Emits per-chunk aggregation + system-metric merge operands into the
distributed aggregation column families. Pairs with
``AggregationMergeOperator`` / ``SystemMetricsMergeOperator`` for
distributed reduction; lives in
``composites/dft/aggregators/aggregation_visitor.h``.

Reorganization Pipeline
-----------------------

Parallel event routing for reorganizing traces by query-based groups. The
``organize`` flow is a streaming pipeline that fans events through visitor
groups, batches output, and periodically flushes group writers
(``GroupWriterTask``) to bound peak memory.

ChunkWriter
~~~~~~~~~~~

Streaming file writer that automatically splits output into chunked files.
Supports optional gzip compression and JSON array wrapping.

.. code-block:: cpp

    #include <dftracer/utils/utilities/fileio/chunk_writer.h>

    using dftracer::utils::utilities::fileio::ChunkWriter;
    using dftracer::utils::utilities::fileio::ChunkWriterConfig;

    ChunkWriterConfig config;
    config.output_dir = "./output";
    config.base_name = "io_events";
    config.chunk_size_bytes = 256 * 1024 * 1024;  // 256 MB per chunk
    config.compress = true;
    config.json_array_wrapper = true;

    ChunkWriter writer(config);
    // Write events — automatically rolls to new chunk file when size exceeded
    // Each chunk is a separate .pfw.gz file

EventRouter
~~~~~~~~~~~

Routes events from source trace files to output groups in parallel using
``AsyncMutex``-protected ``ChunkWriter`` instances. Each group is defined by
a query predicate (from ``ExtractionPlan``), and events matching a group are
written to that group's chunked output.

.. code-block:: cpp

    #include <dftracer/utils/utilities/composites/dft/reorganize/event_router.h>

    using namespace dftracer::utils::utilities::composites::dft::reorganize;

    EventRouterConfig config;
    config.plan = extraction_plan;   // from ReorganizationPlanner
    config.output_dir = "./organized";
    config.chunk_size_bytes = 256 * 1024 * 1024;
    config.compress = true;
    config.executor_threads = 8;

    auto result = co_await route_events(scope, config);
    // result.total_events_written, result.chunks_created, result.output_files

ProvenanceTracker
~~~~~~~~~~~~~~~~~

Tracks source-to-output mapping during reorganization. Records which source
file and line produced each output event, enabling reconstruction of original
traces from reorganized files via ``dftracer_reconstruct``.

ReconstructorUtility
~~~~~~~~~~~~~~~~~~~~

Streaming reconstruction pipeline that inverts the organize pipeline:
plans a reconstruction over a ``.pidx`` provenance store, fans out per-source
read tasks through coroutines and channels, and merges results in
original-order back into the requested output. Defined in
``composites/dft/reorganize/reconstructor_utility.h``.

Comparison
----------

Compare trace metrics between a baseline and variant run. Aggregates
events into time-bucketed windows, computes per-window max across
processes, then mean +/- stdev across windows, and classifies deltas
using Cohen's d effect size (NEGLIGIBLE / SMALL / MEDIUM / LARGE).

ComparisonConfig
~~~~~~~~~~~~~~~~

Configuration can be built from CLI arguments or loaded from a JSON
file with hierarchical node trees. Nodes inherit query filters,
metrics, and percentiles from their parent unless overridden.

.. code-block:: cpp

    #include <dftracer/utils/utilities/composites/dft/comparator/comparison_config.h>

    using namespace dftracer::utils::utilities::composites::dft::comparator;

    // Quick mode from CLI arguments
    auto config = ComparisonConfig::from_cli(
        "./traces_v1", "./traces_v2",
        R"(cat == "POSIX" OR cat == "STDIO")",  // query
        "");                                      // group_by
    config.defaults.time_interval_ms = 1000.0;
    config.resolve();  // propagate defaults down the tree

    // Or load from JSON config file
    std::string error;
    auto config = ComparisonConfig::from_json_file("compare.json", error);
    if (!config) {
        // handle error
    }
    config->resolve();

ComparisonUtility
~~~~~~~~~~~~~~~~~

Joins baseline and variant aggregation outputs, builds the hierarchical
comparison tree (root -> categories -> operations), and computes deltas
with significance classification.

.. code-block:: cpp

    #include <dftracer/utils/utilities/composites/dft/comparator/comparison_utility.h>

    using namespace dftracer::utils::utilities::composites::dft::comparator;

    // Prepare visitor pairs (one per flattened node)
    ComparisonVisitorPair pair;
    pair.baseline = baseline_aggregation_output;
    pair.variant = variant_aggregation_output;
    pair.node = resolved_config_node;

    ComparisonUtilityInput input;
    input.visitors = {pair};
    input.root_node = config.nodes[0];
    input.baseline_file_count = 3;
    input.variant_file_count = 3;

    ComparisonUtility cmp;
    auto output = co_await cmp.process(input);
    // output.result contains the hierarchical NodeResult tree

TreeTableFormatter
~~~~~~~~~~~~~~~~~~

Renders ``ComparisonOutput`` as an ASCII tree table or JSON string.
The table uses dynamic column alignment with UTF-8 display width
awareness.

.. code-block:: cpp

    #include <dftracer/utils/utilities/composites/dft/comparator/tree_table_formatter.h>

    using namespace dftracer::utils::utilities::composites::dft::comparator;

    // Table output to stdout
    FormatterOptions opts;
    opts.use_color = true;
    opts.use_unicode = true;
    TreeTableFormatter formatter(opts);
    formatter.render(stdout, comparison_output);

    // JSON output
    std::string json = formatter.render_json(comparison_output);

    // No-color output (for piping or logging)
    FormatterOptions plain{.use_color = false, .use_unicode = false};
    TreeTableFormatter plain_fmt(plain);
    plain_fmt.render(log_file, comparison_output);

Arrow Export
~~~~~~~~~~~~

``ComparisonOutput::to_arrow()`` flattens the hierarchical tree into a
single Arrow record batch for programmatic analysis. Requires
``DFTRACER_UTILS_ENABLE_ARROW=ON`` at build time.

.. code-block:: cpp

    #include <dftracer/utils/utilities/composites/dft/comparator/comparison_result.h>

    // After running the comparison pipeline:
    auto arrow_result = comparison_output.to_arrow();
    // Columns: node_path, metric_group, metric_name, baseline, variant,
    //          baseline_stdev, variant_stdev, delta, pct_change,
    //          cohens_d, significance, is_regression

See :doc:`/cpp_api/utilities` for the full API reference of all
comparator types.
