:description: Turn raw .pfw traces into parallel-friendly multi-member gzip, or split a large trace into equal chunks, with dftracer_pgzip and dftracer_split.

Compress and split traces
=========================

.. admonition:: Goal
   :class: goal

   Turn raw ``.pfw`` traces into parallel-friendly gzip, or cut a large
   trace into equal chunks. DFTracer traces are gzip only (``.pfw.gz``). The key
   idea throughout is the *multi-member* gzip file: a ``.pfw.gz`` made of several
   independent gzip members so a reader (or indexer) can inflate the members in
   parallel and seek into the middle without decompressing from the start.

Two CLI binaries cover the common cases.

Parallel-compress with dftracer_pgzip
-------------------------------------

``dftracer_pgzip`` compresses every ``.pfw`` file in a directory, splitting each
file into fixed-size chunks and compressing them in parallel as independent gzip
members.

.. code-block:: bash

   # Compress all .pfw in ./traces, 8 MB chunks, max level
   dftracer_pgzip ./traces --chunk-size 8388608 --compression-level 9

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
   dftracer_split ./traces -o ./split --app-name run --chunk-size 64MB

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
     - ``4MB``
     - Output file size, approximate on-disk size. A unit is required, since a
       bare number here once meant megabytes: write ``4MB`` or ``512KB``.
   * - ``-c``, ``--compress``
     - on
     - Compress output with gzip.
   * - ``--verify``
     - off
     - Verify output chunks match input by comparing event IDs.

``dftracer_split`` reads each input file once and builds no index. For split,
the gzip *member* size is ``--checkpoint-size``, because the member is the
pruning and checkpoint unit:

.. code-block:: bash

   # Finer members = finer zoom-in seeks and a larger index
   dftracer_split ./traces -o ./split --checkpoint-size 16777216

``dftracer_genesis_gen_dist`` takes ``--member-size`` (default the checkpoint
size, units allowed such as ``32MB``) for its output members.

See also
--------

- :doc:`../../trace-viewer` - the member is also the indexing and pruning unit.
