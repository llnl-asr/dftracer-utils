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
           PP["PredicateParserUtility"]
           BQ["BloomQueryUtility"]
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
       PP --> BQ
       IDX --> BQ
       Cache --> BQ

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

- Event counts by category, name, and pid:tid
- Timestamp range (min/max)
- Duration statistics (count, sum, min, max, variance via Welford's)
- DDSketch and Log2Histogram for percentile estimation
- Per-name duration breakdowns

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

Index Builders
--------------

Query Utilities
---------------

PredicateParserUtility
~~~~~~~~~~~~~~~~~~~~~~

Parses human-readable predicate strings into structured predicate maps.

Predicate format: ``dimension=value1,value2|dimension2=value3``

- Comma (``,``) separates values within a dimension (OR semantics)
- Pipe (``|``) separates dimensions (AND semantics across dimensions)

.. code-block:: cpp

    PredicateParserInput input;
    input.with_predicate_string("cat=POSIX,STDIO|name=read,write");

    PredicateParserUtility parser;
    auto output = parser.process(input);
    // output.predicates = {
    //   "cat": ["POSIX", "STDIO"],
    //   "name": ["read", "write"]
    // }

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::indexing::PredicateParserInput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::indexing::PredicateParserOutput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::composites::dft::indexing::PredicateParserUtility
   :project: dftracer-utils
   :members:
   :undoc-members:

BloomQueryUtility
~~~~~~~~~~~~~~~~~

Queries bloom indices to identify candidate chunks for a given predicate.

For each dimension in the predicate, checks the bloom filter for that
dimension in each chunk. A chunk is a candidate only if ALL dimensions
have at least one matching value (AND semantics across dimensions,
OR semantics within a dimension).

Returns the list of candidate checkpoint indices, enabling the caller
to skip non-matching chunks entirely.

Tagged ``Parallelizable`` — can query multiple files concurrently.

.. code-block:: cpp

    BloomQueryInput input;
    input.with_idx_path("trace.pfw.gz.idx")
         .with_file_path("trace.pfw.gz")
         .with_predicate("cat", {"POSIX"})
         .with_predicate("name", {"read", "write"});

    BloomQueryUtility query;
    auto output = co_await query.process(input);

    if (!output.file_may_match) {
        // Skip this file entirely
    } else {
        // Only scan candidate checkpoints
        for (auto idx : output.candidate_checkpoints) {
            // Process chunk at checkpoint idx
        }
    }

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::indexing::BloomQueryInput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenstruct:: dftracer::utils::utilities::composites::dft::indexing::BloomQueryOutput
   :project: dftracer-utils
   :members:
   :undoc-members:

.. doxygenclass:: dftracer::utils::utilities::composites::dft::indexing::BloomQueryUtility
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

.. doxygenclass:: dftracer::utils::utilities::reader::TraceReader
   :members:
