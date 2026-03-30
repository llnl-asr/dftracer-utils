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

The indexer module provides sidecar index files (``.idx``) for efficient
random access to compressed trace files. Indexes store:

- **Checkpoints**: Byte offsets and decompression state for random access
- **Bloom filters**: Per-chunk probabilistic membership tests for event filtering
- **Manifests**: Per-checkpoint event line routing tables for reorganization
- **Chunk statistics**: Per-chunk event counts, timestamps, duration distributions

A separate provenance database (``.pidx``) records source-to-output mappings
produced during reorganization.

Getting Started
---------------

Build an index for a compressed trace file using the fluent configuration API:

.. code-block:: cpp

    #include <dftracer/utils/utilities/indexer/index_builder_utility.h>

    using namespace dftracer::utils::utilities::indexer;

    auto config = IndexBuildConfig::for_file("trace.pfw.gz")
        .with_index_dir("/tmp/indexes")
        .with_checkpoint_size(32 * 1024 * 1024)
        .with_bloom(true)
        .with_manifest(true)
        .with_bloom_dimensions(default_bloom_dimensions());

    IndexBuilderUtility builder;
    IndexBuildResult result = co_await builder.process(config);

    if (result.success) {
        // result.idx_path contains the path to the .idx sidecar file
        // result.events_processed, result.chunks_processed hold stats
    }

Once an index exists, open it directly with ``IndexDatabase`` to query bloom
filters, manifests, or chunk statistics:

.. code-block:: cpp

    #include <dftracer/utils/utilities/indexer/index_database.h>

    IndexDatabase db("trace.pfw.gz.idx");
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

- ``with_index_dir(dir)`` -- output directory for ``.idx`` files
- ``with_checkpoint_size(bytes)`` -- decompression checkpoint interval (default 32 MB)
- ``with_index_threshold(bytes)`` -- minimum file size to index
- ``with_force_rebuild(true)`` -- rebuild even if an index already exists
- ``with_bloom(true)`` -- enable per-chunk bloom filter construction
- ``with_manifest(true)`` -- enable per-checkpoint event routing manifests
- ``with_bloom_dimensions(dims)`` -- which JSON fields to index (default: name, cat, pid, tid, hhash, fhash, shash)

IndexBuildResult
----------------

Returned by ``IndexBuilderUtility::process()``. Contains:

- ``idx_path`` -- path to the produced ``.idx`` sidecar
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

SQLite-backed ``.idx`` sidecar file that stores all index data for a trace
file. Schema is additive -- call ``init_base_schema()`` always, then
``init_bloom_schema()`` and/or ``init_manifest_schema()`` as needed.

Provides methods for inserting and querying:

- **Bloom data**: ``insert_chunk_bloom_filter()``, ``query_chunk_bloom_filters()``, ``query_file_bloom_filter()``
- **Chunk statistics**: ``insert_chunk_statistics()``, ``query_chunk_statistics()``, ``query_time_bounds()``
- **Dimension stats**: ``insert_chunk_dimension_stats()``, ``query_chunk_dimension_stats()``
- **Hash resolutions**: ``insert_hash_resolution()``, ``query_resolved_by_hash()``, ``query_hash_by_resolved()``
- **Manifests**: ``insert_event_range()``, ``query_event_ranges()``, ``insert_metadata_lines()``, ``query_metadata_lines()``

.. code-block:: cpp

    IndexDatabase db("trace.pfw.gz.idx");
    db.init_base_schema();
    db.init_bloom_schema();

    int file_id = db.get_or_create_file_info("trace.pfw.gz", file_hash);
    db.insert_chunk_statistics(file_id, checkpoint_idx, stats);

IndexVisitor
------------

Abstract visitor interface for index building passes. Implement this to add
custom indexing logic during the checkpoint-by-checkpoint scan. The builder
calls visitors in order:

1. ``begin(num_checkpoints)`` -- called once before the scan starts
2. ``on_checkpoint(idx)`` -- called at each checkpoint boundary
3. ``on_line(line, checkpoint_idx)`` -- called for every line in the file
4. ``finalize(db, file_id)`` -- called once after the scan to persist results

BloomVisitor
------------

Implements ``IndexVisitor`` to build per-chunk bloom filters and statistics
during the indexing scan. Each checkpoint chunk gets its own set of bloom
filters (one per configured dimension) plus per-chunk event counts and
timestamp/duration distributions.

.. code-block:: cpp

    #include <dftracer/utils/utilities/indexer/visitors/bloom_visitor.h>

    BloomVisitor visitor(bloom_config, {"name", "cat", "pid"});
    visitor.begin(num_checkpoints);

    for (auto& [checkpoint_idx, line] : lines) {
        visitor.on_checkpoint(checkpoint_idx);
        visitor.on_line(line, checkpoint_idx);
    }

    visitor.finalize(db, file_id);

ManifestVisitor
---------------

Implements ``IndexVisitor`` to build per-checkpoint event routing manifests.
During the scan, it collects which lines belong to which ``(cat, name)`` event
pair within each checkpoint. The resulting manifests enable the reorganization
pipeline to selectively read only the lines needed for a given event group.

.. code-block:: cpp

    #include <dftracer/utils/utilities/indexer/visitors/manifest_visitor.h>

    ManifestVisitor visitor;
    visitor.begin(num_checkpoints);
    // ... scan lines ...
    visitor.finalize(db, file_id);

    // Later, query the manifest:
    auto ranges = db.query_event_ranges_for_checkpoint(file_id, checkpoint_idx);

ProvenanceDatabase
------------------

SQLite-backed ``.pidx`` sidecar that records the full reorganization provenance
of an output file: which source files contributed, which checkpoints were read,
and which line ranges map to which output lines.

Schema tables:

- ``file_info`` -- output file identity (path + hash)
- ``provenance_info`` -- key/value metadata (tool version, timestamp, etc.)
- ``provenance_sources`` -- source files that contributed to this output
- ``provenance_group`` -- named predicate groups used during reorganization
- ``provenance_segments`` -- per-checkpoint line range mappings

.. code-block:: cpp

    #include <dftracer/utils/utilities/indexer/provenance_database.h>

    ProvenanceDatabase pdb("output.pfw.gz.pidx");
    pdb.init_schema();

    int fid = pdb.get_or_create_file_info("output.pfw.gz", file_hash);
    pdb.insert_info("version", "1.0");
    pdb.insert_source(fid, 0, "source.pfw.gz", num_checkpoints);
    pdb.insert_segment(0, checkpoint_idx, out_start, out_end, event_count);

    // Query provenance later
    auto sources = pdb.query_sources(fid);
    auto segments = pdb.query_segments(source_idx);
