DFTracer Indexing System
========================

Bloom filter indexing and manifest building for fast event lookup in trace files.
All classes are in the ``dftracer::utils::utilities::composites::dft::indexing`` namespace.

The indexing system creates sidecar files (``.idx`` unified index, ``.pidx``
for provenance) that enable sub-second event filtering without scanning
entire trace files. Bloom filters provide probabilistic set membership testing
per chunk, while chunk statistics enable predicate pushdown.

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

       subgraph Storage["Sidecar Files"]
           IDX[".idx<br/>(Unified Index)"]
           PIDX[".pidx<br/>(Provenance Index)"]
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
        // This chunk MAY contain "read" events — scan it
    }
    if (!filter.possibly_contains("close")) {
        // This chunk definitely does NOT contain "close" — skip it
    }

    // Serialize for storage in .idx SQLite database
    auto blob = filter.serialize();

    // Deserialize from storage
    auto restored = BloomFilter::from_blob(blob.data(), blob.size());

.. doxygenclass:: dftracer::utils::utilities::composites::dft::indexing::BloomFilter
   :project: dftracer-utils
   :members:
   :undoc-members:

BloomFilterCache
~~~~~~~~~~~~~~~~

Thread-safe bounded LRU cache for deserialized bloom filters.

Avoids repeated deserialization of bloom filters from the ``.idx`` database
during query execution. Cache keys are ``(idx_path, dimension, checkpoint_idx)``.

When the cache is full, all entries are evicted (simple reset strategy).

.. doxygenclass:: dftracer::utils::utilities::composites::dft::indexing::BloomFilterCache
   :project: dftracer-utils
   :members:
   :undoc-members:

Chunk Statistics
----------------

Per-chunk aggregated statistics stored alongside bloom filters in the ``.idx``
sidecar. Used for predicate pushdown (e.g., skip chunks where max timestamp
is before the query range) and for summary queries without full scans.

Includes:

- Timestamp range (min/max)
- Duration statistics (count, sum, min, max, variance via Welford's)
- DDSketch and Log2Histogram for percentile estimation
- Per-name duration breakdowns

Event counts by category, name, and pid:tid are stored in the
``chunk_dimension_stats`` table (see below) and reconstructed into
``ChunkStatistics`` fields on read-back.

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::indexing::ChunkStatistics
   :project: dftracer-utils
   :members:
   :undoc-members:

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
    config.expected_entries_per_chunk = 2048;
    config.false_positive_rate = 0.01;
    config.build_manifest = true;

    // Add custom dimensions (dot-path into JSON events)
    config.extra_dimensions = {"args.filename", "args.size"};

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::indexing::ChunkIndexerConfig
   :project: dftracer-utils
   :members:
   :undoc-members:

ChunkIndexerUtility
~~~~~~~~~~~~~~~~~~~

Per-chunk indexer (parallelizable).

Reads events from a byte range, builds bloom filters for each configured
dimension, computes chunk statistics, and optionally builds manifest
line groups for event-level routing.

Supports incremental indexing: if ``existing_state`` is provided, only
missing dimensions are indexed (detected via config hash comparison).

Tagged ``Parallelizable`` — multiple instances run concurrently across chunks.

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::indexing::ChunkIndexerInput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::indexing::ChunkIndexerOutput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::composites::dft::indexing::ChunkIndexerUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

Supporting Types
~~~~~~~~~~~~~~~~

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::indexing::IndexedDimensions
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::indexing::ChunkIndexState
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::indexing::EventLineGroup
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::indexing::MetadataLineGroup
   :project: dftracer-utils
   :members:
   :undoc-members:

Chunk Dimension Stats
---------------------

Per-dimension per-chunk metadata stored in the ``chunk_dimension_stats``
SQLite table. Each row tracks one dimension (e.g., "cat", "name", "pid")
within one chunk.

Stores:

- ``distinct_count`` — number of unique values
- ``min_value`` / ``max_value`` — range (numeric-aware comparison for uint/int/double types)
- ``value_counts`` — compressed binary BLOB mapping values to counts (NULL when compressed size exceeds 4 KB cap)
- ``value_type`` — "string", "uint", "int", or "double"

Used by ``ChunkPrunerUtility`` for three-tier chunk skipping:
dictionary lookup, range check, bloom filter fallback.

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::indexing::ChunkDimensionStats
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::indexing::ChunkDimensionStatsResult
   :project: dftracer-utils
   :members:
   :undoc-members:

Index Builders
--------------

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

.. doxygenclass:: dftracer::utils::utilities::common::query::Query
   :project: dftracer-utils
   :members:
   :undoc-members:

ChunkPrunerUtility
------------------

Replaces ``BloomQueryUtility``. Accepts a ``Query`` and determines which
chunks are candidates using three-tier evaluation:

1. **Dictionary** — exact lookup in ``chunk_dimension_stats.value_counts``
2. **Min/Max range** — check against ``chunk_dimension_stats.min_value``/``max_value`` (numeric-aware)
3. **Bloom filter** — probabilistic probe with hash resolution for fhash/hhash/shash

The pruner walks the Query AST recursively:

- ``AND`` → intersect candidate sets
- ``OR`` → union candidate sets
- ``NOT`` → complement via dictionary exclusivity (requires value_counts; without dictionary, cannot safely skip)

Tagged ``Parallelizable`` — can query multiple files concurrently.

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

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::indexing::ChunkPrunerInput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::indexing::ChunkPrunerOutput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::composites::dft::indexing::ChunkPrunerUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

Database Schemas
----------------

IndexDatabase
~~~~~~~~~~~~~
Manages the unified ``.idx`` SQLite sidecar file with additive schema
(checkpoints + bloom filters + statistics + manifest).

.. doxygenclass:: dftracer::utils::utilities::indexer::IndexDatabase
   :members:

ProvenanceDatabase
~~~~~~~~~~~~~~~~~~
Manages the ``.pidx`` SQLite sidecar file for reorganization provenance.

.. doxygenclass:: dftracer::utils::utilities::indexer::ProvenanceDatabase
   :members:

IndexBuilder
~~~~~~~~~~~~
Single-pass index builder that decompresses once and builds all index
data (checkpoints, bloom filters, manifest) via the visitor pattern.

.. doxygenclass:: dftracer::utils::utilities::indexer::IndexBuilderUtility
   :members:

TraceReader
~~~~~~~~~~~
Smart reader that auto-selects between sequential decompression and
indexed random access based on ``.idx`` file presence.

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

.. doxygenclass:: dftracer::utils::utilities::reader::TraceReader
   :members:

.. doxygenstruct:: dftracer::utils::utilities::reader::ReadConfig
   :project: dftracer-utils
   :members:
