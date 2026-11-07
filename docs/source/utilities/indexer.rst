:description: The indexing infrastructure that builds a .dftindex store - checkpoints, bloom filters, statistics, aggregation - from one decompression pass.

Indexer
=================

Unified indexing and reading infrastructure for compressed trace files. Builds a sidecar ``.dftindex`` store that enables efficient random access, bloom-filter-accelerated queries, and distributed aggregation, all from a single decompression pass.

.. code-block:: cpp

   #include <dftracer/utils/index/indexer.h>

Overview
--------

The indexer writes a shared ``.dftindex`` store, in which each process,
distributed workers included, writes its own data file. Its data belongs
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
  source that the build evaluates, one native frame per row set (for
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
the caller's ``coro::Scope``. The same calls are in the C ABI
(:doc:`../c_api/indexer`).

.. code-block:: cpp

   #include <dftracer/utils/index/indexer.h>

   using namespace dftracer::utils::index;

   IndexerOptions options;
   options.index_dir = "/data/.dftindex";
   options.parallelism = 16;

   Indexer indexer = Indexer::open({"a.pfw.gz", "b.pfw.gz", "c.pfw.gz"}, options);
   IndexStatus status = indexer.build();  // or co_await indexer.build(scope)

The batch builder and resolver behind it are private
(``src/dftracer/utils/index/build/``) and are not installed. The
command-line tools use them directly for member normalization and bounded
sub-batches, which ``Indexer`` does not offer yet.

Python API
----------

**Indexer:**

``Indexer`` takes ``directory`` (scanned for trace files) or an explicit
``files`` list; at least one must be given. ``ensure_indexed()`` resolves
which files need work and builds them (checkpoint + bloom tiers by default).
``extensions`` picks the pruning extensions and ``schema`` names the record
schema. ``manifest()`` and ``explain(duql)`` inspect the index,
``rebuild_extension(name)`` and ``drop_extension(name)`` act on one extension,
and ``rowset(name)`` returns a stored row set as a DataFrame.

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

The Python bindings read trace data through ``TraceViewer``, a lazy
builder over the index.

.. code-block:: python

   from dftracer.utils import TraceViewer

   # files is a path, list of paths, or a directory (scanned recursively)
   tv = TraceViewer(["trace.pfw.gz"])
   df = tv.filter("name == 'read'").collect()  # runs the scan, returns a DataFrame

See Also
--------

- :doc:`/cli` - Command-line tools (``dftracer_index``)
- :doc:`/cpp_api/indexer` - C++ API reference for indexing classes
