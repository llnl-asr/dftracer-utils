:description: Convert query results at the edge: move a native DataFrame or Series to pandas, polars, Arrow, NumPy, or Parquet, zero-copy where possible.

Get data out
============

Everything upstream stays in the native columnar engine. When you need the data
in another tool - pandas, polars, an Arrow file - convert at the edge. The
crossing is the Arrow C Data Interface, so a flat non-null numeric column moves
zero-copy.

To pandas, polars, or Arrow (Python)
------------------------------------

A collected ``DataFrame`` (or a single ``Series``) converts on demand:

.. code-block:: python

   tbl = df.to_arrow()       # pyarrow.Table (zero-copy)
   pdf = df.to_pandas()      # pandas DataFrame
   pldf = df.to_polars()     # polars DataFrame

   arr = df["dur"].to_arrow()    # pyarrow.Array
   np_arr = df["dur"].to_numpy() # NumPy (zero-copy for flat non-null numeric)

There is no native ``to_parquet``. Write Parquet through the Arrow edge:

.. code-block:: python

   import pyarrow.parquet as pq
   pq.write_table(df.to_arrow(), "out.parquet")
   # or: df.to_polars().write_parquet("out.parquet")

The native ``DataFrame`` also implements ``__arrow_c_stream__`` and ``Series``
implements ``__arrow_c_array__``, so any Arrow-aware library can pull the data
directly.

Read Arrow IPC files in parallel (Python)
-------------------------------------------

To read a batch of already-written ``.arrow`` IPC files back in, use
``read_arrow_files_parallel`` from the C extension. It fans the reads out
across the :doc:`Runtime <../../api/runtime>` and returns per-file results
plus totals:

.. code-block:: python

   from dftracer.utils.dftracer_utils_ext import read_arrow_files_parallel

   result = read_arrow_files_parallel(["a.arrow", "b.arrow"])
   result["total_rows"]      # rows across every file
   result["files_read"]      # files that succeeded
   result["files_failed"]    # files that raised

   for fr in result["file_results"]:
       fr["path"], fr["success"], fr["total_rows"]
       fr["batches"]          # list of Arrow-capsule batches

Pass an explicit ``runtime=`` (a :class:`~dftracer.utils.Runtime`) to reuse a
runtime you already created; it uses the module-level default otherwise. Each
entry in ``fr["batches"]`` implements the Arrow C Data Interface
(``__arrow_c_array__``); wrap the list in ``dftracer.utils.arrow.ArrowTable``
(a pure-Python helper, not part of the ``dftracer.utils`` top-level namespace)
to get ``to_pandas`` / ``to_polars``:

.. code-block:: python

   import pyarrow as pa
   from dftracer.utils.arrow import ArrowTable

   batches = [pa.record_batch(b) for fr in result["file_results"] for b in fr["batches"]]
   table = ArrowTable(batches).to_pandas()

This function is built only when Arrow IPC support is compiled in
(``DFTRACER_UTILS_ENABLE_ARROW_IPC``); there is no Python-level write path for
IPC files. To write one, use the C++ ``IpcWriter`` below, or convert through
pyarrow (``pa.ipc.new_file(...).write_table(df.to_arrow())``).

The Arrow C Data Interface (C++)
--------------------------------

The zero-copy bridge is ``to_arrow`` / ``from_arrow`` on ``Series`` and
``DataFrame``, in the installed public header
``dftracer/utils/dataframe/arrow.h`` (namespace ``dftracer::utils::dataframe``,
built when ``DFTRACER_UTILS_ENABLE_ARROW`` is set). ``to_arrow`` returns an
``OwnedArrow`` - a move-only RAII owner of the paired ``ArrowSchema`` /
``ArrowArray`` whose buffers alias the column's - so the caller never juggles
release callbacks by hand. A ``DataFrame`` exports as a struct array with one
child per column. The header pulls in the Arrow C Data Interface struct
definitions itself, so a consumer needs no Arrow library to include it.

.. code-block:: cpp

   #include <dftracer/utils/dataframe/arrow.h>

   using namespace dftracer::utils::dataframe;

   OwnedArrow a = df.to_arrow();                       // zero-copy export
   // ... hand (a.schema(), a.array()) to any Arrow consumer ...
   DataFrame back = DataFrame::from_arrow(a.schema(), a.array());

   // A single column round-trips the same way.
   OwnedArrow col = df.column("dur").to_arrow();
   Series s = Series::from_arrow(col.schema(), col.array());

Write an Arrow IPC file (C++)
-----------------------------

To persist columnar data as a ``.arrow`` file readable by pyarrow / polars, use
the async ``IpcWriter`` (``dftracer/utils/utilities/common/arrow/ipc_writer.h``,
namespace ``dftracer::utils::utilities::common::arrow``, built when
``DFTRACER_UTILS_ENABLE_ARROW_IPC`` is set). It consumes finished record batches
(``ArrowExportResult``) and supports buffer-level zstd compression; the sequence
is ``open`` then ``write_batch`` (one or more) then ``close``, and it runs on the
executor.

.. code-block:: cpp

   #include <dftracer/utils/utilities/common/arrow/ipc_writer.h>

   using namespace dftracer::utils::utilities::common::arrow;

   IpcWriter writer;
   co_await writer.open("out.arrow");         // DEFAULT_ARROW_IPC_COMPRESSION
   co_await writer.write_batch(batch);        // ArrowExportResult
   co_await writer.close();

Write a new trace
-----------------

To export matching events back out as a re-indexable ``.pfw.gz`` trace (rather
than a table), use the trace query's own export path - ``export_json`` (C++;
``sink_json`` on the Python ``TraceViewer``) for newline-delimited events or
``export_trace`` for the parallel multi-member writer. See :doc:`../../trace-viewer`.

See also
--------

- :doc:`../data/dataframe` and :doc:`../data/series` for the ops that run before
  you convert.
- :doc:`../../columnar-engine` for how the Arrow bridge and encodings work.
- :doc:`../../cpp_api/arrow` for the generated Arrow utility reference.
