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

By default ``to_pandas()`` gives NumPy dtypes, so an integer column with nulls
becomes ``float64`` with NaN. Two options change that, neither needs pyarrow:

.. code-block:: python

   pdf = df.to_pandas(nullable=True)               # Int64, Float64, boolean, string; a null is pd.NA
   pdf = df.to_pandas(index=["name", "hash"])      # those columns become the (Multi)Index

``nullable=True`` gives pandas' own nullable dtypes for integer, float, bool and
string columns (``Int8`` to ``Int64``, ``UInt8`` to ``UInt64``, ``Float32``,
``Float64``, ``boolean``, ``string``); a float NaN stays NaN and only a null is
``pd.NA``. A column of another type converts as usual. ``index`` moves the named
columns into the pandas index in the order given (a flat index for one name, a
MultiIndex for several); an unknown name raises ``KeyError`` and a repeated one
``ValueError``. The two options combine; ``nullable=True`` with ``arrow=True`` is
an error. ``Series.to_pandas`` takes ``nullable`` as well. The frame itself has
no row index: keep the key columns in the frame and ask for the index here.

The conversions to pandas, NumPy and Python lists run natively in one pass per
column, with no pyarrow and no Python object per row for numbers and bools. On
5,000,000 rows, ``to_pandas(nullable=True)`` of 20 integer and float columns
with nulls takes about 55 ms, a bool column with nulls 2 ms, and ``to_list`` of
an integer column with nulls about 56 ms (Python needs 50 ms to build such a
list). A string column fills a NumPy object array or a list directly, and rows
with the same text share one ``str``, so a column of repeated names allocates
once per distinct name. A dictionary-encoded column keeps its nulls.
``to_arrow`` exports a dictionary-encoded column as an Arrow dictionary array,
which pandas and polars read through Arrow as a categorical column;
``to_list``, ``to_numpy``, ``to_pandas`` and ``to_polars`` give plain strings.
``to_ipc`` writes a dictionary-encoded column as its values.
A view column exports as an Arrow ``string_view`` or ``binary_view`` array
with zero copy (pyarrow 16 or later), and ``from_arrow`` imports such arrays
the same way.
``to_polars`` reads the native Arrow stream through polars' own Arrow import
(polars 1.3 or later) and needs no pyarrow. ``to_pandas`` builds a string
column from its Arrow array when pandas keeps strings in Arrow (pandas 3 with
pyarrow). ``to_list``, ``to_numpy`` and the pandas string columns decode each
dictionary value once, not once per row.
``benchmarks/groupby_analyzer_bench.py --ops to_pandas`` measures it.

Write Parquet with ``to_parquet`` (or the polars-shaped ``write_parquet``), or
through the Arrow edge:

.. code-block:: python

   df.to_parquet("out.parquet")
   # or: import pyarrow.parquet as pq; pq.write_table(df.to_arrow(), "out.parquet")
   # or: df.to_polars().write_parquet("out.parquet")

``to_parquet`` and ``write_parquet`` also write the pandas metadata block, so
``pandas.read_parquet`` restores the nullable extension dtype of every integer,
bool and string column that holds a null (``Int64``, ``UInt8``, ``boolean``,
``string``); a column with no null reads as pandas reads it by default
(``int64``, ``bool``), and a float with nulls reads as ``float64`` with NaN.
A frame has no memory of the dtype it came from, so a pandas ``Int64`` column
with no null comes back as ``int64``; read with
``dtype_backend="numpy_nullable"`` or cast it if you need ``Int64`` regardless.

``DataFrame.from_parquet`` reads a file or a directory of part files. A
directory written by Dask with its default index holds an extra column,
``__null_dask_index__``; ``from_parquet`` drops it (the file's pandas metadata
lists it as an index column), and keeps any other stored index as a column. The
part files of such a directory, whose strings are ``large_string``, concatenate
with ``concat`` like any other frames.

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
than a table), use the trace query's own export path - ``sink_json`` (C++ and
Python) for newline-delimited events or ``sink_trace`` (C++) / ``export_trace``
(Python) for the parallel multi-member writer. See :doc:`../../trace-viewer`.

See also
--------

- :doc:`../data/dataframe` and :doc:`../data/series` for the ops that run before
  you convert.
- :doc:`../../columnar-engine` for how the Arrow bridge and encodings work.
- :doc:`../../cpp_api/arrow` for the generated Arrow utility reference.
