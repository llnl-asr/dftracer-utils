:description: Pick the right entry point - TraceViewer, View, DataFrame, a plugin, or the C ABI - for the job before writing code.

Choose the right API
======================

.. admonition:: Goal
   :class: goal

   Pick an entry point before writing code, instead of discovering partway
   through that a different layer would have been simpler. dftracer-utils exposes
   the same engine at several altitudes; each is the right choice for a different
   job.

Decision table
---------------

.. list-table::
   :header-rows: 1
   :widths: 30 45 25

   * - If you want to...
     - Use
     - See
   * - Query trace files directly: filter, group-by/aggregate, export, from
       Python
     - ``TraceViewer``
     - :doc:`../trace-viewer`, :doc:`analysis/views`
   * - Query trace files directly, from C++, or need the lower-level escape
       hatches (``ViewSession``, custom folds)
     - ``View``
     - :doc:`analysis/views`
   * - Compute over data you already hold in memory (already collected, or
       imported from pandas/polars/Arrow/NumPy) - arithmetic, joins, reshape,
       time windows
     - ``DataFrame`` / ``Series``
     - :doc:`data/dataframe`, :doc:`data/series`, :doc:`data/ingest`
   * - Add a new aggregation or analysis that rides the engine's fused scan,
       authored in Python, without writing C++
     - a JIT plugin (``@jit.plugin``)
     - :doc:`../jit`
   * - Add a new aggregation or analysis that needs native performance, custom
       state, or capabilities beyond the JIT subset (joins, nested maps,
       inter-plugin channels, custom I/O)
     - a compiled C plugin
     - :doc:`../plugins`, :doc:`plugins/inter-plugin-comms`
   * - Call the engine from a C program, or from another language via its C
       FFI, with no C++ in your own translation unit
     - the C ABI (``dftu_duql``, ``dftu_series``/``dftu_dataframe``,
       ``dftu_plugin``)
     - :doc:`core/c-abi`
   * - Serve query results over HTTP, or open the interactive timeline UI
     - ``dftracer_server``
     - :doc:`serving/http-server`, :doc:`../trace-viewer`
   * - Run a flamegraph, counters or an aggregation across many nodes / MPI
       ranks
     - ``dftracer_view`` under ``mpirun``
     - :doc:`scale/mpi`

How to read the table
-----------------------

**Trace query vs in-memory compute** is the first fork. ``TraceViewer``/``View``
own reading ``.pfw.gz`` files: they apply predicate pushdown against the index
(:doc:`core/indexing`) so a filtered query can skip whole chunks unread. A
``DataFrame``/``Series`` has no file-reading concept of its own - it is the
result type a view query (or any other ``from_*`` importer) hands you, and
where the SIMD arithmetic, joins, and reshaping live once the data is in
memory. A view's terminal (``collect()``) is exactly the seam between the two:
before it you are describing a scan over trace files, after it you are
computing over a native columnar table.

**Python vs C++** is not really a choice between different capabilities -
``TraceViewer`` is a thin wrapper over the same ``View`` engine, and the
``DataFrame``/``Series`` Python classes wrap the same C++ columnar engine. Pick
based on where the rest of your code lives, not on features; anything one
surface can do, the underlying engine can do from the other language too
(with C++ exposing a few extra low-level escape hatches, like the
``map_batches`` custom fold, that have no Python binding).

**Plugin vs duql** is about where the logic runs. The duql filter language and
``group_by``/``agg`` vocabulary (:doc:`core/duql`, :doc:`analysis/aggregation`)
cover filtering and the built-in aggregate ops without writing any new code.
Reach for a plugin only when the built-in vocabulary cannot express what you
need - custom per-event state, a join across two of your own maps, or an
output shape group-by does not produce. Within plugins, prefer the JIT layer
first: it compiles a statically-typed Python method to the same native plugin
ABI, so you get native speed without writing C. Drop to a hand-written C
plugin when you need a capability outside the JIT's typed subset (see the
vocabulary and typing notes in :doc:`../jit`).

**The C ABI** is not a reduced or interop-only surface - it is the same engine
the C++ and Python layers call into. Reach for it directly when your caller is
C, or another language's FFI, and you do not want a C++ compiler in the
dependency chain.

See also
--------

- :doc:`end-to-end` for a full workflow built from these pieces, start to
  finish.
- :doc:`../concepts/architecture` for how the layers above compose internally.
