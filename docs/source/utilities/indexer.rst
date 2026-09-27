:description: The indexing infrastructure that builds a .dftindex store - checkpoints, bloom filters, statistics, aggregation - from one decompression pass.

Indexer
=================

Unified indexing and reading infrastructure for compressed trace files. Builds a sidecar ``.dftindex`` RocksDB store (and optional flat-file SSTs) that enables efficient random access, bloom-filter-accelerated queries, and distributed aggregation, all from a single decompression pass.

.. code-block:: cpp

   #include <dftracer/utils/index/indexer.h>
   #include <dftracer/utils/utilities/reader/trace_reader.h>

Overview
--------

The indexer writes a shared ``.dftindex`` RocksDB store (or, for distributed
builds, SST files that one atomic ingest adds to the store). Its data belongs
to extensions, each with a manifest entry per file:

- ``core.members`` - gzip member boundaries and file metadata, for random
  access
- ``zonemap``, ``bloom``, ``counts``, ``postings`` - the pruning extensions,
  chosen with ``IndexerOptions::extensions``
- ``dft.stats`` - chunk and file statistics, built with the pruning
  extensions
- ``dft.metadata`` - per chunk, the number of metadata (``ph="M"``) records
  and of context records (thread and process names, ``PR``, ``CM``), built
  with the pruning extensions
- ``core.rowset`` - the rows of each row set of the file's record schema
  source that the build evaluates, one Arrow IPC frame per row set (for
  dftracer: ``files``, ``hosts``, ``strings`` and ``ranks``)

A file's data and its manifest entries are written in one atomic write, so an
interrupted build leaves the file unindexed rather than half indexed. The
aggregation and system-metrics families are written through the same writes
as merge operands.

Indexer
-------

The public entry point for building an index. ``Indexer::open`` takes trace
files and directories; ``status`` reports which files are ready, ``build``
indexes the files that are missing a requested tier or changed since they
were indexed, ``rebuild`` re-indexes every file, and ``files`` lists what the
index holds. ``manifest`` and ``explain`` inspect the extensions, and
``rebuild_extension`` and ``drop_extension`` act on one of them. Each call has a blocking form and an async form that runs in
the caller's ``CoroScope``. The same calls are in the C ABI
(:doc:`../c_api/indexer`).

.. code-block:: cpp

   #include <dftracer/utils/index/indexer.h>

   using namespace dftracer::utils::index;

   IndexerOptions options;
   options.index_dir = "/data/.dftindex";
   options.parallelism = 16;

   Indexer indexer = Indexer::open({"a.pfw.gz", "b.pfw.gz", "c.pfw.gz"}, options);
   IndexStatus status = indexer.build();  // or co_await indexer.build(scope)

The batch builder, resolver and the distributed SST writer behind it are
private (``src/dftracer/utils/index/build/``) and are not installed. The
command-line tools use them directly for member normalization, bounded
sub-batches and SST output, which ``Indexer`` does not offer yet.

IndexDatabase
-------------

RocksDB-backed index store (the ``.dftindex`` root). Each kind of index data
belongs to an extension, and the manifest records, per file, which extensions
are built; data without a current manifest entry is never read.

.. code-block:: cpp

   #include <dftracer/utils/index/store/index_database.h>

   using namespace dftracer::utils::index::store;

   IndexDatabase db("trace.pfw.gz.dftindex");
   db.init_schema();  // idempotent; writes the format version

   // Writes go through a writer context
   auto writer = db.begin_write();

   // Read-only queries
   int fid = db.get_file_info_id("trace.pfw.gz");
   bool has_bloom = db.extension_current(fid, IndexExtension::BLOOM);

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

   #include <dftracer/utils/index/build/index_visitor.h>

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
(:cpp:class:`dftracer::utils::index::schemas::dft::BloomCore`, in
``index/schemas/dft/bloom_core.h``) is a stateless per-chunk harvest/persist
core driven from the shared POD batch scan by an internal ``BloomFold``,
and aggregation merge operands are produced the same way by an internal
``AggregationFold``. Both fold drivers live under ``src/`` and are not
public API. ``IndexVisitor`` remains the extension point for
line-callback-style consumers of the checkpoint scan (for example the
index-to-view and sink-writer drivers), but there are no built-in visitor
classes to subclass directly.

Low-level CheckpointIndexerFactory
----------------------------------

Creates checkpoint indexers with automatic format detection (currently
GZIP only; an unrecognized format returns ``nullptr``). Used internally by
the index build pipeline.

.. code-block:: cpp

   #include <dftracer/utils/index/gzip/checkpoint_indexer_factory.h>

   using namespace dftracer::utils::index::gzip;

   // Passing an empty index_path auto-generates a .dftindex root next to
   // the input file.
   auto indexer = CheckpointIndexerFactory::create(
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

   # Every flat args key is indexed by default; name nested ones too
   from dftracer.utils import BloomConfig

   with Indexer(files=["trace.pfw.gz"], require_bloom=BloomConfig(fields=["io.off"])) as indexer:
       indexer.ensure_indexed()

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
   df = tv.filter("name == 'read'").collect()  # runs the scan, returns a DataFrame

See Also
--------

- :doc:`/cli` - Command-line tools (``dftracer_index``)
- :doc:`/cpp_api/indexer` - C++ API reference for indexing classes
