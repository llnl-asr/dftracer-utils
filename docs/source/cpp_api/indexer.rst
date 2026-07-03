Indexer Components
==================

.. seealso::

   For complete class and member documentation, see the
   :doc:`API Reference <api/utilities/indexer>`.

Indexing and searching functionality for compressed trace files.
All classes are in the ``dftracer::utils::utilities::indexer`` namespace.

.. mermaid:: ../_generated/indexer.mmd

Overview
--------

The indexer module provides a root-local ``.dftindex`` RocksDB store for
efficient random access to compressed trace files. The store keeps index data
in dedicated column families:

- **Checkpoints**: Byte offsets and decompression state for random access
- **Bloom filters**: Per-chunk probabilistic membership tests for event filtering
- **Manifests**: Per-checkpoint event line routing tables for reorganization
- **Chunk statistics**: Per-chunk event counts, timestamps, duration distributions

Reorganization provenance (source-to-output mappings) lives in the same shared
``.dftindex`` store, in its own column family.

Getting Started
---------------

Build an index for a compressed trace file using the fluent configuration API:

.. code-block:: cpp

    #include <dftracer/utils/utilities/indexer/index_builder_utility.h>

    using namespace dftracer::utils::utilities::indexer;

    std::vector<std::string> dims(DEFAULT_BLOOM_DIMENSIONS.begin(),
                                  DEFAULT_BLOOM_DIMENSIONS.end());

    auto config = IndexBuildConfig::for_file("trace.pfw.gz")
        .with_index_dir("/tmp/indexes")
        .with_checkpoint_size(32 * 1024 * 1024)
        .with_manifest(true)
        .with_bloom_dimensions(dims);

    IndexBuilderUtility builder;
    IndexBuildResult result = co_await builder.process(config);

    if (result.success) {
        // result.index_path contains the path to the .dftindex store
        // result.events_processed, result.chunks_processed hold stats
    }

Once an index exists, open it directly with ``IndexDatabase`` to query bloom
filters, manifests, or chunk statistics:

.. code-block:: cpp

    #include <dftracer/utils/utilities/indexer/index_database.h>

    IndexDatabase db(result.index_path);
    int file_id = db.find_file("trace.pfw.gz");

    // Query time bounds across all chunks
    auto bounds = db.query_time_bounds(file_id);

    // Query bloom filters for a specific dimension
    auto blooms = db.query_chunk_bloom_filters(file_id, "name");

    // Query per-checkpoint event routing manifests
    auto ranges = db.query_event_ranges(file_id);

IndexBuildConfig
----------------

Fluent builder for configuring an index build pass. Start with the
static factory ``for_file()`` and chain ``with_*`` methods:

- ``with_index_dir(dir)`` -- directory holding the ``.dftindex`` store
- ``with_checkpoint_size(bytes)`` -- decompression checkpoint interval (default 32 MB)
- ``with_force_rebuild(true)`` -- rebuild even if an index already exists
- ``with_manifest(true)`` -- enable per-checkpoint event routing manifests
- ``with_bloom_config(cfg)`` -- ``ChunkIndexerConfig`` controlling bloom parameters and indexed dimensions
- ``with_bloom_dimensions(dims)`` -- which JSON fields to index (see ``DEFAULT_BLOOM_DIMENSIONS``: name, cat, pid, tid, hhash, fhash, shash)

``DEFAULT_BLOOM_DIMENSIONS`` (7 fields) is the default indexed set;
``DEFAULT_EXTRA_DIMENSIONS`` (ret, count, offset, epoch, step) names the
additional argument fields available for dimension statistics. The config
also carries an ``extra_dft_visitors`` list of ``DftEventVisitor`` references
that are driven alongside the built-in bloom/manifest visitors during the
single indexing scan.

IndexBuildResult
----------------

Returned by ``IndexBuilderUtility::process()``. Contains:

- ``index_path`` -- path to the produced ``.dftindex`` store
- ``file_path`` -- the indexed trace file
- ``success`` / ``was_skipped`` / ``index_created`` -- outcome flags
- ``events_processed`` / ``chunks_processed`` / ``total_lines`` -- build statistics
- ``error_message`` -- non-empty on failure

IndexBuilderUtility
-------------------

Coroutine-based utility that drives the full index build pipeline. Extends
``Utility<IndexBuildConfig, IndexBuildResult, tags::NeedsContext>`` and requires
an executor context to run. Call ``process(config)`` inside a coroutine to
build the index asynchronously.

.. code-block:: cpp

    IndexBuilderUtility builder;
    IndexBuildResult result = co_await builder.process(config);

IndexDatabase
-------------

RocksDB-backed handle over the root-local ``.dftindex`` store that holds all
index data across column families. Call ``init_schema()`` once (idempotent) to
create the column families. Writes go through a batched writer context
obtained from ``begin_write()``; the read-only query API is called directly.

Read query API (selected):

- **Bloom data**: ``query_chunk_bloom_filters(file_id, dimension)``, ``query_file_bloom_filter(file_id, dimension)``
- **Chunk statistics**: ``query_chunk_statistics(file_id)``, ``query_time_bounds(file_id)``
- **Dimension stats**: ``query_chunk_dimension_stats(file_id)``
- **Hash tables**: ``resolve_hash(type, hash)``, ``resolve_name_to_hash(type, name)`` where ``type`` is ``IndexDatabase::HashType`` (``FILE``/``HOST``/``STRING``/``PROC``)
- **Manifests**: ``query_event_ranges(file_id)``, ``query_metadata_lines(file_id)``
- **File lookup**: ``find_file(path)``, ``get_file_info_id(path)``

.. code-block:: cpp

    IndexDatabase db(result.index_path);
    db.init_schema();

    // Batched writes go through a writer context.
    auto writer = db.begin_write();  // IndexDatabaseWriterContext
    // ... visitors emit records into *writer ...

    // Reads use the query API directly.
    int file_id = db.find_file("trace.pfw.gz");
    auto stats = db.query_chunk_statistics(file_id);

IndexVisitor
------------

Abstract visitor interface for index building passes. Implement this to add
custom indexing logic during the checkpoint-by-checkpoint scan. The builder
calls visitors in order:

1. ``begin(std::size_t num_checkpoints)`` -- called once before the scan starts
2. ``on_checkpoint(std::size_t idx)`` -- ``CoroTask<void>``, called at each checkpoint boundary
3. ``on_chunk(const char* data, std::size_t len, std::size_t checkpoint_idx)`` -- ``CoroTask<void>``; the default splits the chunk into lines and calls ``on_line``
4. ``on_line(std::string_view line, SharedLineBuffer buffer, std::size_t checkpoint_idx)`` -- called for every line; store ``buffer`` to keep ``line`` data alive (zero-copy)
5. ``finalize(IndexDatabaseWriterContext& writer, int file_id)`` -- called once after the scan to persist results

Optional overrides support backpressure and buffering: ``flush()`` and
``drain_pending()`` return ``CoroTask<void>``, and ``wants_drain()`` (default
false, polled after each ``on_line``) hints that ``drain_pending()`` should run
to apply backpressure when a downstream channel is full. ``SharedLineBuffer``
is ``std::shared_ptr<std::string>``; keep it to hold the ``on_line`` view alive.

Indexer
-------

The only low-level indexer is ``internal::Indexer`` (in the
``dftracer::utils::utilities::indexer::internal`` namespace), an abstract base
for the per-archive indexer implementations (gzip, tar.gz). Application code
should use ``IndexBuilderUtility`` or ``IndexBatchBuilderUtility`` rather than
the internal indexer directly.

IndexBatchBuilderUtility
------------------------

Batched variant of ``IndexBuilderUtility`` that processes a list of files in
parallel against a shared ``IndexDatabaseWriterContext``, yielding an
``IndexBuildBatchResult`` with aggregated metrics. Configured via
``IndexBuildBatchConfig`` (file list, parallelism, checkpoint size, bloom and
manifest toggles, shared sink).

IndexBuildBatchConfig
~~~~~~~~~~~~~~~~~~~~~

Configuration struct for ``IndexBatchBuilderUtility``: file slices, output
directory, checkpoint size, bloom/manifest flags, and the shared
``IndexBatchSink`` (typically an ``IndexDatabaseWriterContext``) that
receives encoded batches from all workers.

IndexDatabaseWriterContext
--------------------------

Implements ``IndexBatchSink`` and owns a thread-safe writer pipeline into a
RocksDB-backed ``IndexDatabase``. Workers in ``IndexBatchBuilderUtility``
submit encoded index batches to this context, which serializes them into
checkpoint, bloom, manifest, and statistics column families.

BloomVisitor
------------

Implements ``DftEventVisitor`` to build per-chunk bloom filters and
statistics during the indexing scan. Each checkpoint chunk gets its own set
of bloom filters (one per configured dimension) plus per-chunk event counts
and timestamp/duration distributions.

The ``DftEventVisitor`` interface is ``begin(num_checkpoints)``,
``on_checkpoint(checkpoint_idx)``, and ``on_event(const EventRecord&)``;
visitors are driven by ``DftEventDispatcher``, which parses each line once and
fans the resulting ``EventRecord`` out to all registered visitors (so a single
scan feeds bloom, manifest, and aggregation at once). ``BloomVisitor::finalize``
takes an ``IndexDatabaseWriterContext&``.

.. code-block:: cpp

    #include <dftracer/utils/utilities/composites/dft/visitors/bloom_visitor.h>

    BloomVisitor visitor(bloom_config, {"name", "cat", "pid"});
    visitor.begin(num_checkpoints);
    // ... DftEventDispatcher calls on_checkpoint(idx) / on_event(record) ...

    auto writer = db.begin_write();
    visitor.finalize(*writer, file_id);

ManifestVisitor
---------------

Implements ``DftEventVisitor`` to build per-checkpoint event routing
manifests. During the scan, it collects which lines belong to which
``(cat, name)`` event pair within each checkpoint. The resulting manifests
enable the reorganization pipeline to selectively read only the lines needed
for a given event group. ``ManifestVisitor::finalize`` takes an
``IndexBatchSink&`` (an ``IndexDatabaseWriterContext`` is one).

.. code-block:: cpp

    #include <dftracer/utils/utilities/composites/dft/visitors/manifest_visitor.h>

    ManifestVisitor visitor;
    visitor.begin(num_checkpoints);
    // ... DftEventDispatcher drives on_checkpoint / on_event ...

    auto writer = db.begin_write();
    visitor.finalize(*writer, file_id);

    // Later, query the manifest:
    auto ranges = db.query_event_ranges_for_checkpoint(file_id, checkpoint_idx);

IndexResolverUtility
--------------------

Resolves a directory or file list into a set of ``FileWorkItem`` entries by
opening or building per-file indexes and emitting line-range work items
suitable for parallel scan / aggregation / replay pipelines. Defined in
``dftracer/utils/utilities/composites/dft/indexing/index_resolver_utility.h``.

ProvenanceDatabase
------------------

RocksDB-backed handle over the same shared ``.dftindex`` store that records
the full reorganization provenance of an output file: which source files
contributed, which checkpoints were read, and which line ranges map to which
output lines. It is not a separate ``.pidx`` sidecar file. Use
``determine_provenance_index_path(data_path, index_dir)`` to resolve the store
path for a data file.

Provenance schema (logical entries within the provenance column family):

- ``file_info`` -- output file identity (path + hash)
- ``provenance_info`` -- key/value metadata (tool version, timestamp, etc.)
- ``provenance_sources`` -- source files that contributed to this output
- ``provenance_group`` -- named predicate groups used during reorganization
- ``provenance_segments`` -- per-checkpoint line range mappings

.. code-block:: cpp

    #include <dftracer/utils/utilities/indexer/provenance_database.h>

    ProvenanceDatabase pdb(
        determine_provenance_index_path("output.pfw.gz", index_dir));
    pdb.init_schema();

    int fid = pdb.get_or_create_file_info("output.pfw.gz", file_hash);

    // Inserts are batched inside a transaction.
    pdb.begin_transaction();
    pdb.insert_info(fid, "version", "1.0");
    pdb.insert_source(fid, /*source_idx=*/0, "source.pfw.gz", num_checkpoints);
    pdb.insert_segment(fid, /*source_idx=*/0, source_checkpoint, segment_seq,
                       out_line_start, out_line_end, event_count);
    pdb.commit_transaction();

    // Query provenance later
    auto sources = pdb.query_sources(fid);
    auto segments = pdb.query_segments(fid, /*source_idx=*/0);
