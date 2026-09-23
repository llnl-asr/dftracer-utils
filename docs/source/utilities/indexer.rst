:description: The indexing infrastructure that builds a .dftindex store - checkpoints, bloom filters, statistics, aggregation - from one decompression pass.

Indexer
=================

Unified indexing and reading infrastructure for compressed trace files. Builds a sidecar ``.dftindex`` RocksDB store (and optional flat-file SSTs) that enables efficient random access, bloom-filter-accelerated queries, and distributed aggregation, all from a single decompression pass.

.. code-block:: cpp

   #include <dftracer/utils/utilities/indexer/index_builder_utility.h>
   #include <dftracer/utils/utilities/reader/trace_reader.h>

Overview
--------

The indexer writes column families into a shared ``.dftindex`` RocksDB store
(or, for distributed builds, a content-addressed SST staging directory that
is ingested into the store):

- **Checkpoints** - byte offsets and decompression dictionaries for random access
- **Bloom filters** - per-chunk bloom filters for fast event filtering (optional)
- **Chunk statistics** - per-chunk event counts, duration distributions (optional)
- **Aggregation / system metrics** - distributed aggregation CFs populated via
  ``SstFileWriter::Merge`` operands

SST files staged on disk are **content-addressed** (FNV-1a 64-bit fingerprint
over the SST payload) so identical SSTs produced by different ranks collapse
to a single ingest, and re-ingesting is idempotent. String IDs in the
``names`` and ``cats`` CFs are deterministic FNV-1a hashes so the same name
maps to the same id across processes.

IndexBatchBuilderUtility
------------------------

Builds many files in a single pipelined pass. Parses files in parallel
(``parallelism`` workers) and routes their parsed artifacts (bloom rows,
aggregation merge operands, extra-visitor SSTs) to a
write phase. Supports batched flushing (``flush_every_files``) to bound
peak memory, distributed SST sinks via ``sink_factory`` / ``sink_commit``,
preassigned file ids, and per-file gzip-member slicing for cross-rank file
splitting (the MPI driver pre-scans each ``.pfw.gz`` for member boundaries
and assigns disjoint ``[member_begin, member_end)`` ranges to ranks).

.. code-block:: cpp

   #include <dftracer/utils/utilities/indexer/index_builder_utility.h>

   IndexBuildBatchConfig cfg;
   cfg.file_paths = {"a.pfw.gz", "b.pfw.gz", "c.pfw.gz"};
   cfg.index_dir = "/data/.dftindex";
   cfg.parallelism = 16;
   cfg.rebuild_root_summaries = true;
   cfg.flush_every_files = 8;

   auto batch = co_await IndexBatchBuilderUtility::process(scope,
       std::make_shared<IndexBuildBatchConfig>(std::move(cfg)));

IndexDatabaseWriterContext
--------------------------

Implements ``IndexBatchSink`` over a coordinator-owned RocksDB store: each
batch's parsed artifacts are buffered, then committed atomically via
``WriteBatch``. ``IndexDatabaseSstWriterContext`` is the SST-staging
variant used by the distributed indexer; its outputs are content-addressed
SST files later ingested into the coordinator store.

IndexResolverUtility
--------------------

Resolves the index directory for a given trace file, building the index on
demand when ``auto_build_index`` is set. Lives under ``trace/indexing/``
(namespace ``dftracer::utils::trace::indexing``) because it depends on the
DFT aggregation config.

.. code-block:: cpp

   #include <dftracer/utils/trace/indexing/index_resolver_utility.h>

IndexDatabase
-------------

RocksDB-backed index store (part of the ``.dftindex`` root) with an
additive, idempotent schema across column families.

.. code-block:: cpp

   #include <dftracer/utils/utilities/indexer/index_database.h>

   using namespace dftracer::utils::utilities::indexer;

   IndexDatabase db("trace.pfw.gz.dftindex");
   db.init_schema();  // idempotent; sets up all column families

   // Writes go through a writer context
   auto writer = db.begin_write();

   // Read-only queries
   int fid = db.get_file_info_id("trace.pfw.gz");
   bool has_bloom = db.has_bloom_data(fid);

TraceReader
-----------

Unified reader for gzip-compressed trace files (``.pfw.gz``). Auto-selects between sequential decompression and indexed random access based on ``.dftindex`` presence.

Two methods cover all reading modes:

- ``read_lines(ReadConfig)`` - returns parsed ``Line`` objects (``string_view``, zero-copy)
- ``read_raw(ReadConfig)`` - returns raw byte spans (``std::span<const char>``)

``ReadConfig`` controls range (line or byte), alignment, and buffering.

.. code-block:: cpp

   #include <dftracer/utils/utilities/reader/trace_reader.h>

   using namespace dftracer::utils::utilities::reader;

   TraceReader reader({.file_path = "trace.pfw.gz"});

   // Read all lines (default)
   auto gen = reader.read_lines();
   while (auto line = co_await gen.next()) {
       // line->content is string_view, valid until next iteration
   }

   // Line range
   ReadConfig rc;
   rc.start_line = 100;
   rc.end_line = 200;
   auto range = reader.read_lines(rc);

   // Raw bytes - line-aligned, multi-line chunks (fastest for bulk processing)
   auto raw = reader.read_raw();
   while (auto chunk = co_await raw.next()) {
       // chunk is std::span<const char>
   }

   // Raw bytes - single line per yield
   ReadConfig single;
   single.line_aligned = true;
   single.multi_line = false;
   auto line_bytes = reader.read_raw(single);

   // Raw bytes - no line awareness
   ReadConfig raw_cfg;
   raw_cfg.line_aligned = false;
   auto bytes = reader.read_raw(raw_cfg);

   // Byte range
   ReadConfig byte_range;
   byte_range.start_byte = 0;
   byte_range.end_byte = 1024 * 1024;
   auto chunk_gen = reader.read_raw(byte_range);

**ReadConfig to StreamType mapping:**

.. list-table::
   :header-rows: 1

   * - ``read_raw`` flags
     - Internal StreamType
   * - ``line_aligned=true, multi_line=true`` (default)
     - ``MULTI_LINES_BYTES``
   * - ``line_aligned=true, multi_line=false``
     - ``LINE_BYTES``
   * - ``line_aligned=false``
     - ``BYTES``

IndexVisitor
------------

Interface for processing decompressed chunks during index building. Implementations receive each chunk (a batch of lines) and its checkpoint index.

.. code-block:: cpp

   #include <dftracer/utils/utilities/indexer/index_visitor.h>

   class IndexVisitor {
   public:
       virtual ~IndexVisitor() = default;
       virtual void begin(std::size_t num_checkpoints) = 0;
       virtual coro::CoroTask<void> on_checkpoint(std::size_t checkpoint_idx) = 0;
       virtual coro::CoroTask<void> on_chunk(const char* data, std::size_t len,
                                             std::size_t checkpoint_idx) = 0;
       virtual void finalize(IndexDatabaseWriterContext& writer, int file_id) = 0;
   };

Index building itself no longer goes through public ``IndexVisitor``
subclasses. Bloom filters and chunk statistics are populated by the
internal fold-based scan core: ``BloomCore``
(:cpp:class:`dftracer::utils::trace::visitors::BloomCore`, in
``trace/visitors/bloom_core.h``) is a stateless per-chunk harvest/persist
core driven from the shared POD batch scan by an internal ``BloomFold``,
and aggregation merge operands are produced the same way by an internal
``AggregationFold``. Both fold drivers live under ``src/`` and are not
public API. ``IndexVisitor`` remains the extension point for
line-callback-style consumers of the checkpoint scan (for example the
index-to-view and sink-writer drivers), but there are no built-in visitor
classes to subclass directly.

Low-level IndexerFactory
------------------------

Creates checkpoint indexers with automatic format detection (currently
GZIP only; an unrecognized format returns ``nullptr``). Used internally by
the index build pipeline.

.. code-block:: cpp

   #include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>

   using namespace dftracer::utils::utilities::indexer::internal;

   // Passing an empty index_path auto-generates a .dftindex root next to
   // the input file.
   auto indexer = IndexerFactory::create(
       "trace.pfw.gz",     // Input file
       "",                 // Output index path (empty = auto-generate)
       32 * 1024 * 1024,   // Checkpoint size (32MB)
       true                // Force rebuild
   );

   co_await indexer->build_async();

   std::size_t num_lines = indexer->get_num_lines();
   auto members = indexer->get_members();  // std::vector<GzipMemberRecord>

Python API
----------

**Indexer:**

``Indexer`` takes ``directory`` (scanned for trace files) or an explicit
``files`` list; at least one must be given. ``ensure_indexed()`` resolves
which files need work and builds them (checkpoint + bloom tiers by default).

.. code-block:: python

   from dftracer.utils import Indexer

   # Checkpoint + bloom build over an explicit file list
   with Indexer(files=["trace.pfw.gz"]) as indexer:
       indexer.build()

   # Resolve, then build only if needed
   with Indexer(files=["trace.pfw.gz"], build_bloom=True) as indexer:
       status = indexer.ensure_indexed()
       print(status.ready, status.needs_work)

   # Single-file checkpoint-level details (lines, max bytes, members)
   with Indexer(files=["trace.pfw.gz"]) as indexer:
       indexer.build()
       ckpt = indexer.get_checkpoint_indexer("trace.pfw.gz")
       print(f"Lines: {ckpt.get_num_lines()}")

   # With explicit Runtime for thread pool control
   from dftracer.utils import Runtime

   with Runtime(threads=8) as rt:
       with Indexer(files=["trace.pfw.gz"], build_bloom=True, runtime=rt) as indexer:
           indexer.build()  # uses rt's thread pool

**TraceViewer:**

The Python bindings read trace data through ``TraceViewer`` (a lazy,
Arrow-native builder over the index), not through a Python ``TraceReader``
- ``TraceReader`` is a C++-only class (see above).

.. code-block:: python

   from dftracer.utils import TraceViewer

   # files is a path, list of paths, or a directory (scanned recursively)
   tv = TraceViewer(["trace.pfw.gz"])
   df = tv.filter("name == 'read'").collect().collect()  # LazyFrame, then DataFrame

See Also
--------

- :doc:`/cli` - Command-line tools (``dftracer_index``)
- :doc:`/cpp_api/indexer` - C++ API reference for indexing classes
