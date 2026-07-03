DFTracer Indexing System
========================

.. seealso::

   For complete class and member documentation, see the
   :doc:`API Reference <api/utilities/composites/dft/indexing>`.


Bloom filter indexing and manifest building for fast event lookup in trace files.
All classes are in the ``dftracer::utils::utilities::composites::dft::indexing`` namespace.

The indexing system writes into a single root-local ``.dftindex`` RocksDB
store (index data and reorganization provenance share the same store) that
enables sub-second event filtering without scanning entire trace files. Bloom
filters provide probabilistic set membership testing per chunk, while chunk
statistics enable predicate pushdown.

.. mermaid::

   graph LR
       subgraph Input
           Files["Trace Files<br/>(.pfw.gz)"]
       end

       subgraph Indexing["Parallel Chunk Indexing"]
           CI1["ChunkIndexerUtility"]
           CI2["ChunkIndexerUtility"]
           CIN["ChunkIndexerUtility"]
       end

       subgraph Storage[".dftindex RocksDB Store"]
           IDX["Index column families<br/>(checkpoints, bloom, stats, manifest)"]
           PIDX["Provenance column family"]
       end

       subgraph Query["Query Path"]
           QP["Query Parser"]
           CP["ChunkPrunerUtility"]
           Cache["BloomFilterCache"]
       end

       Files --> CI1
       Files --> CI2
       Files --> CIN
       CI1 --> IDX
       CI2 --> IDX
       CIN --> IDX
       CI1 --> PIDX
       CI2 --> PIDX
       CIN --> PIDX
       QP --> CP
       IDX --> CP
       Cache --> CP

Bloom Filter
------------

Probabilistic set membership data structure for fast event filtering.

Each bloom filter tracks values for a single dimension (e.g., event name,
category, PID) within a single chunk. The filter answers "does this chunk
possibly contain events with value X?" with configurable false positive rate.

**Serialization format:** ``[num_hashes: 4B] [num_entries: 4B] [num_bits: 4B] [bit_array]``

Usage example:

.. code-block:: cpp

    // Create a filter expecting 1000 entries with 1% FP rate
    BloomFilter filter(1000, 0.01);

    // Add values during indexing
    filter.add("read");
    filter.add("write");
    filter.add("open");

    // Query during search
    if (filter.possibly_contains("read")) {
        // This chunk MAY contain "read" events - scan it
    }
    if (!filter.possibly_contains("close")) {
        // This chunk definitely does NOT contain "close" - skip it
    }

    // Serialize for storage in the .dftindex RocksDB store
    auto blob = filter.serialize();

    // Deserialize from storage
    auto restored = BloomFilter::from_blob(blob.data(), blob.size());

BloomFilterCache
~~~~~~~~~~~~~~~~

Thread-safe bounded LRU cache for deserialized bloom filters.

Avoids repeated deserialization of bloom filters from the ``.dftindex`` store
during query execution. Cache keys are ``(index_path, dimension, checkpoint_idx)``;
file-level filters use ``FILE_LEVEL_SENTINEL`` (``UINT64_MAX``) as the index.

When the cache is full, all entries are evicted (simple reset strategy).

.. code-block:: cpp

    BloomFilterCache cache;  // default 10000 entries
    if (auto bf = cache.get(index_path, "name", checkpoint_idx)) {
        if (bf->possibly_contains("read")) { /* scan chunk */ }
    } else {
        BloomFilter loaded = BloomFilter::from_blob(blob.data(), blob.size());
        cache.put(index_path, "name", checkpoint_idx, loaded);
    }

Chunk Statistics
----------------

Per-chunk aggregated statistics stored alongside bloom filters in the
``.dftindex`` store. Used for predicate pushdown (e.g., skip chunks where max timestamp
is before the query range) and for summary queries without full scans.

Includes:

- Timestamp range (min/max)
- Duration statistics (count, sum, min, max, variance via Welford's)
- DDSketch and Log2Histogram for percentile estimation
- Per-name duration breakdowns

Event counts by category, name, and pid:tid are stored in the
``chunk_dim_stats`` column family (see below) and reconstructed into
``ChunkStatistics`` fields on read-back.

Chunk Indexer
-------------

ChunkIndexerConfig
~~~~~~~~~~~~~~~~~~

Configuration for per-chunk indexing.

Controls which dimensions are indexed (name, category, PID, TID, hashes),
bloom filter parameters, and whether to build manifest indices.

.. code-block:: cpp

    ChunkIndexerConfig config;
    config.index_name = true;
    config.index_cat = true;
    config.index_pid = true;
    config.index_tid = true;
    config.index_hhash = true;   // host-hash dimension
    config.index_fhash = true;   // file-hash dimension
    config.index_shash = true;   // string-hash dimension
    config.expected_entries_per_chunk = 2048;
    config.false_positive_rate = 0.01;
    config.build_manifest = true;
    config.value_counts_cap = 4096;  // 0 disables dictionaries

    // Add custom dimensions (dot-path into JSON events)
    config.extra_dimensions = {"args.filename", "args.size"};

``compute_hash()`` returns a ``std::size_t`` fingerprint of the config; the
indexer compares it against a chunk's stored hash to detect config changes
and drive incremental re-indexing.

ChunkIndexerUtility
~~~~~~~~~~~~~~~~~~~

Per-chunk indexer (parallelizable).

Reads events from a byte range, builds bloom filters for each configured
dimension, computes chunk statistics, and optionally builds manifest
line groups for event-level routing.

Supports incremental indexing: if ``existing_state`` is provided, only
missing dimensions are indexed (detected via config hash comparison).

Multiple instances run concurrently across chunks.

.. code-block:: cpp

    coro::CoroTask<ChunkIndexerOutput> process(const ChunkIndexerInput&);

    ChunkIndexerUtility indexer;
    auto input = ChunkIndexerInput()
                     .with_file_path("trace.pfw.gz")
                     .with_index_path("./traces/.dftindex")
                     .with_byte_range(start, end)
                     .with_checkpoint_idx(0)
                     .with_config(config);
    ChunkIndexerOutput out = co_await indexer.process(input);
    // out.bloom_filters (per-dimension), out.statistics (ChunkStatistics),
    // out.event_line_groups / out.metadata_line_groups when build_manifest.

Supporting Types
~~~~~~~~~~~~~~~~

Chunk Dimension Stats
---------------------

Per-dimension per-chunk metadata stored in the ``chunk_dim_stats``
RocksDB column family. Each entry tracks one dimension (e.g., "cat", "name", "pid")
within one chunk.

Stores:

- ``distinct_count`` - number of unique values
- ``min_value`` / ``max_value`` - range (numeric-aware comparison for uint/int/double types)
- ``value_counts`` - compressed binary BLOB mapping values to counts (NULL when compressed size exceeds 4 KB cap)
- ``value_type`` - "string", "uint", "int", or "double"

Used by ``ChunkPrunerUtility`` for three-tier chunk skipping:
dictionary lookup, range check, bloom filter fallback.

Visitors
--------

The single-pass index builder decompresses each file once and fans the parsed
events out to a set of ``DftEventVisitor`` instances via ``DftEventDispatcher``
(see :doc:`dft_aggregators`). Each visitor implements ``on_event(const
EventRecord&)`` and contributes one facet of the index. Both visitors below
support ``create_parallel_slice`` / ``merge_parallel_slice`` so the dispatcher
can parse slices of a chunk concurrently.

BloomVisitor
~~~~~~~~~~~~

Builds the per-chunk bloom filters, ``ChunkStatistics``, and
``ChunkDimensionStats`` for the fixed dimensions (name, cat, pid, tid, hhash,
fhash, shash) plus any configured extra dimensions. Defined in
``dft/visitors/bloom_visitor.h``.

``finalize`` writes to a live RocksDB handle; the SST-build path uses the
``*_to_sink`` overloads instead.

.. code-block:: cpp

    #include <dftracer/utils/utilities/composites/dft/visitors/bloom_visitor.h>

    BloomVisitor bloom(config, config.extra_dimensions);
    // driven by DftEventDispatcher: begin() -> on_checkpoint() -> on_event()...
    bloom.finalize(writer_context, file_id);  // IndexDatabaseWriterContext&

ManifestVisitor
~~~~~~~~~~~~~~~

Builds the manifest line groups: per-(cat, name) and per-metadata-type lists
of line numbers, plus the set of observed pids, enabling event-level routing
without a full rescan. Defined in ``dft/visitors/manifest_visitor.h``.

``finalize`` writes through an ``IndexBatchSink`` (not a live DB handle).

.. code-block:: cpp

    #include <dftracer/utils/utilities/composites/dft/visitors/manifest_visitor.h>

    ManifestVisitor manifest;
    // driven by DftEventDispatcher across the file...
    manifest.finalize(sink, file_id);         // IndexBatchSink&

Query Language
--------------

Generic JSON filtering via a recursive descent parser and AST evaluator.
All classes are in the ``dftracer::utils::utilities::common::query`` namespace.

Query DSL syntax:

- **Comparison**: ``field == "value"``, ``field != "value"``, ``field > 100``
- **Logical**: ``expr and expr``, ``expr or expr``, ``not expr``
- **Membership**: ``field in ["a", "b"]``, ``field not in ["a"]``
- **Grouping**: ``(expr)``
- **Field paths**: dotted notation for nested JSON (``args.level``)

Keywords (``and``, ``or``, ``not``, ``in``, ``true``, ``false``) are
case-insensitive. String values are case-sensitive.

.. code-block:: cpp

    using namespace dftracer::utils::utilities::common::query;

    // Parse a query string
    auto result = Query::from_string(R"(cat == "POSIX" and dur > 1000)");
    if (result) {
        Query query = std::move(*result);

        // Evaluate against a JSON event
        JsonValue event = ...;
        bool matches = query.evaluate(event);

        // Evaluate against a typed key-value map
        ValueMap fields = {{"cat", std::string("POSIX")}, {"dur", uint64_t(2000)}};
        bool matches2 = query.evaluate(fields);
    }

    // Throw on parse error
    Query q = parse_or_throw(R"(name in ["read", "write"])");

ChunkPrunerUtility
------------------

Replaces ``BloomQueryUtility``. Accepts a ``Query`` and determines which
chunks are candidates using three-tier evaluation:

1. **Dictionary** - exact lookup in ``chunk_dim_stats`` value counts
2. **Min/Max range** - check against ``chunk_dim_stats`` min_value/max_value (numeric-aware)
3. **Bloom filter** - probabilistic probe with hash resolution for fhash/hhash/shash

The pruner walks the Query AST recursively:

- ``AND`` -> intersect candidate sets
- ``OR`` -> union candidate sets
- ``NOT`` -> complement via dictionary exclusivity (requires value_counts; without dictionary, cannot safely skip)

Can query multiple files concurrently.

.. code-block:: cpp

    using namespace dftracer::utils::utilities::composites::dft::indexing;

    auto query = common::query::parse_or_throw(R"(cat == "POSIX" and dur > 1000)");

    ChunkPrunerInput input{idx_path, file_path, std::move(query), &cache};
    ChunkPrunerUtility pruner;
    auto output = co_await pruner.process(input);

    if (!output.file_may_match) {
        // Skip this file entirely
    } else {
        for (auto idx : output.candidate_checkpoints) {
            // Only scan candidate chunks
        }
    }

Databases
---------

IndexDatabase
~~~~~~~~~~~~~
RocksDB-backed handle over the root-local ``.dftindex`` store. Index data is
spread across column families (checkpoints, bloom filters, statistics,
dimension stats, manifest, hash tables, ...); ``init_schema()`` creates them
idempotently.

ProvenanceDatabase
~~~~~~~~~~~~~~~~~~
RocksDB-backed handle over the same shared ``.dftindex`` store, exposing the
reorganization provenance data (its own column family, not a separate file).

IndexBuilder
~~~~~~~~~~~~
Single-pass index builder that decompresses once and builds all index
data (checkpoints, bloom filters, manifest) via the visitor pattern.

TraceReader
~~~~~~~~~~~
Smart reader that auto-selects between sequential decompression and
indexed random access based on ``.dftindex`` store presence.

When ``ReadConfig.query`` is set, ``read_lines()`` parses the query once,
runs ``ChunkPrunerUtility`` for chunk skipping (when an index exists),
and evaluates per-event for all paths (indexed, gzip, plain file).

.. code-block:: cpp

    TraceReaderConfig cfg{.file_path = "trace.pfw.gz"};
    TraceReader reader(cfg);

    ReadConfig rc;
    rc.query = R"(cat == "POSIX" and dur > 1000)";

    auto gen = reader.read_lines(rc);
    while (auto line = co_await gen.next()) {
        // Only matching lines yielded
    }

