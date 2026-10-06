:description: Turn raw .pfw traces into parallel-friendly multi-member gzip, or split a large trace into equal chunks, with dftracer_pgzip and the C++ API.

Compress and split traces
=========================

.. admonition:: Goal
   :class: goal

   Turn raw ``.pfw`` traces into parallel-friendly gzip, or cut a large
   trace into equal chunks. DFTracer traces are gzip only (``.pfw.gz``). The key
   idea throughout is the *multi-member* gzip file: a ``.pfw.gz`` made of several
   independent gzip members so a reader (or indexer) can inflate the members in
   parallel and seek into the middle without decompressing from the start.

Two CLI binaries cover the common cases; a C++ API sits underneath them.

Parallel-compress with dftracer_pgzip
-------------------------------------

``dftracer_pgzip`` compresses every ``.pfw`` file in a directory, splitting each
file into fixed-size chunks and compressing them in parallel as independent gzip
members.

.. code-block:: bash

   # Compress all .pfw in ./traces, 8 MB chunks, max level
   dftracer_pgzip -d ./traces --chunk-size 8388608 -l 9

.. list-table::
   :header-rows: 1
   :widths: 30 20 50

   * - Flag
     - Default
     - Meaning
   * - ``-d``, ``--directory``
     - ``.``
     - Directory of ``.pfw`` files to compress.
   * - ``-l``, ``--compression-level``
     - ``6``
     - Compression level (0-12).
   * - ``--chunk-size``
     - ``4194304`` (4 MB)
     - Gzip member size in bytes. A member is cut at the first line end at or
       after this size, so a member is never split inside an event line.

Split a trace with dftracer_split
---------------------------------

``dftracer_split`` cuts traces into equal-sized output chunks, compressed by
default. Use it to break one huge trace into files sized for parallel
processing.

.. code-block:: bash

   # Split ./traces into ~64 MB gzip chunks under ./split, named run_*
   dftracer_split -d ./traces -o ./split -n run -s 64

.. list-table::
   :header-rows: 1
   :widths: 30 20 50

   * - Flag
     - Default
     - Meaning
   * - ``-d``, ``--directory``
     - ``.``
     - Directory of trace files.
   * - ``-o``, ``--output``
     - ``./split``
     - Output directory for the chunks.
   * - ``-n``, ``--app-name``
     - ``app``
     - Application name used in output filenames.
   * - ``-s``, ``--chunk-size``
     - ``4``
     - Output file size, approximate on-disk size. A bare number is MB. A
       suffixed value such as ``512KB`` is absolute.
   * - ``-c``, ``--compress``
     - on
     - Compress output with gzip.
   * - ``--verify``
     - off
     - Verify output chunks match input by comparing event IDs.

For split, the gzip *member* size is the ``--checkpoint-size`` (the shared
indexing flag), because the member is the pruning and checkpoint unit:

.. code-block:: bash

   # Finer members = finer zoom-in seeks and a larger index
   dftracer_split -d ./traces -o ./split --checkpoint-size 16777216

Compression in C++
------------------

Namespace ``dftracer::utils::utilities::fileio``. To compress one file into a
multi-member ``.pfw.gz``, use ``FileCompressorUtility``. Its input carries the
member size directly; the output is multi-member so it can be inflated and
indexed in parallel.

.. code-block:: cpp

   #include <dftracer/utils/utilities/fileio/file_compressor_utility.h>
   using namespace dftracer::utils::utilities::fileio;

   auto input = FileCompressionUtilityInput::from_file(
       "trace-0.pfw", /*compression_level=*/6, /*member_size=*/16 * 1024 * 1024)
       .with_output("trace-0.pfw.gz");

   FileCompressorUtility compress;
   // operator() is a coroutine returning Result<Output>; run it on a scope
   // (see the concurrency guide) and unwrap with -> or .value().
   auto out = co_await compress(input);
   double ratio = out->compression_ratio();

To write trace events out as a rotating series of multi-member chunks, use
``ChunkWriter``. ``ChunkWriterConfig`` controls the chunk size, the per-member
byte size, and compression. ``member_size_bytes`` must be positive. It gives
multi-member gzip closed at line boundaries.

.. code-block:: cpp

   #include <dftracer/utils/utilities/fileio/chunk_writer.h>
   using namespace dftracer::utils::utilities::fileio;

   ChunkWriterConfig cfg;
   cfg.output_dir = "./out";
   cfg.base_name  = "run";
   cfg.chunk_size_bytes  = 256 * 1024 * 1024;
   cfg.member_size_bytes = 16 * 1024 * 1024;   // multi-member
   cfg.compression_level = 6;

   ChunkWriter writer(cfg);
   co_await writer.open();
   for (auto& line : lines) co_await writer.write_line(line);
   co_await writer.close();

GzipLineWriter
~~~~~~~~~~~~~~

``GzipLineWriter`` (header ``utilities/fileio/gzip_line_writer.h``) is the one
writer behind ``ChunkWriter`` and ``FileCompressorUtility``. The View, indexed
and aggregate exports, the Python export sink, the rechunker, the plugin trace
writer, ``dftracer_split``, ``dftracer_pgzip``, ``dftracer_genesis_gen_dist``
and the fake trace writer write through it as well.

The writer turns appended lines into gzip members of at least ``member_size``
bytes. A member is cut at the first ``\n`` at or after that size. Members are
compressed in parallel. In ordered mode (the default) they reach the file in
line order. With ``ordered = false`` each worker writes its own members in no
total order, which suits parallel file systems. Unordered mode takes
``producer()`` handles so several coroutines append without sharing state.

.. list-table::
   :header-rows: 1
   :widths: 30 20 50

   * - ``GzipWriterOptions`` field
     - Default
     - Meaning
   * - ``member_size``
     - the checkpoint size
     - Minimum bytes per member. Must be positive.
   * - ``level``
     - ``6``
     - Compression level.
   * - ``workers``
     - ``0``
     - Compress workers. ``0`` uses the machine's parallelism.
   * - ``memory_budget``
     - ``0``
     - Bytes the writer may hold. ``0`` is a third of available memory.
       A call that would pass the budget waits.
   * - ``ordered``
     - ``true``
     - ``false`` lets each worker write its own members.

``append`` takes whole lines that end with ``\n``. ``append_fmt`` and
``append_json`` format one line in place. The ``{}`` characters are a
placeholder that takes the next argument. Every other brace is literal, so JSON
braces need no doubling, and ``{{}}`` writes an empty object ``{}``. The writer
adds the newline, so a format has none. ``append_json`` escapes string
arguments for JSON and ``append_fmt`` copies them.

.. code-block:: cpp

   #include <dftracer/utils/utilities/fileio/gzip_line_writer.h>
   using namespace dftracer::utils::utilities::fileio;

   GzipWriterOptions opts;
   opts.member_size = 16 * 1024 * 1024;
   opts.memory_budget = 512ull * 1024 * 1024;

   auto writer = (co_await GzipLineWriter::open("out.pfw.gz", opts)).value();
   co_await writer.append_json<R"({"name":{},"dur":{},"args":{{}}})">(
       "read", 42);
   auto summary = (co_await writer.close()).value();

``close()`` returns a ``WriteSummary`` with the member and line counts.
``close(tail)`` appends final bytes that need no newline. The first error is
kept and returned by every later call. ``GzipLineWriterBlocking`` offers
``append``, ``append_with``, ``cut`` and ``close`` without coroutines.

``dftracer_genesis_gen_dist`` takes ``--member-size`` (default the checkpoint
size, units allowed such as ``32MB``) for its output members.

The low-level codec is ``GzipMemberCompressor`` (namespace
``dftracer::utils::utilities::fileio::compress``, header
``compress/libdeflate_gzip.h``): a one-shot whole-buffer gzip over libdeflate.
Multi-member output is produced by calling it once per member;
``compress_member_into`` reuses the output buffer's capacity across calls on hot
paths.

See also
--------

- :doc:`reading-files` - stream the members back in, line by line.
- :doc:`../../trace-viewer` - the member is also the indexing and pruning unit.
- :doc:`../pipelines/patterns` - how to run the coroutine ``operator()`` calls.
