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

By default ``to_pandas()`` gives NumPy dtypes for columns without nulls.
An integer, bool or string column that holds nulls gives pandas' own nullable dtype (``Int64``, ``boolean``, ``string``), so ``DataFrame.from_pandas(df).to_pandas()`` keeps those dtypes.
On pandas 3 a string column with nulls is the default ``str`` dtype, which holds missing values already.
A float column keeps NaN, and dates and times keep their NumPy types.
A dictionary-encoded string column gives ``category``.
``from_pandas`` stores a ``category`` column with string categories as a dictionary-encoded column, and ``to_pandas`` gives back only the categories that are used.
With pyarrow installed the categories keep the dictionary order and no string is built per row; without it the categories come out sorted.
Two options change the result further, and neither needs pyarrow:

.. code-block:: python

   pdf = df.to_pandas(nullable=True)               # Int64, Float64, boolean, string; a null is pd.NA
   pdf = df.to_pandas(index=["name", "hash"])      # those columns become the (Multi)Index

``nullable=True`` gives pandas' own nullable dtypes for integer, float, bool and
string columns (``Int8`` to ``Int64``, ``UInt8`` to ``UInt64``, ``Float32``,
``Float64``, ``boolean``, ``string``) even when the column holds no nulls, and ``category`` for a dictionary column; a float NaN stays NaN and only a null is
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
``to_pandas`` gives ``category``, ``to_polars`` gives ``Categorical``, and ``to_list`` and ``to_numpy`` give plain strings.
``from_polars`` reads a ``Categorical`` or ``Enum`` column as a dictionary-encoded column, so an ``Enum`` comes back as ``Categorical``.
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

Read Arrow IPC files (Python)
-----------------------------

To read already-written ``.arrow`` IPC files back in, open them with pyarrow
and import the table with ``DataFrame.from_arrow``:

.. code-block:: python

   import pyarrow as pa
   from dftracer.utils import DataFrame

   tables = [pa.ipc.open_file(path).read_all() for path in ["a.arrow", "b.arrow"]]
   df = DataFrame.from_arrow(pa.concat_tables(tables))

``df.write_ipc(path)`` writes the frame as an Arrow IPC stream, and
``df.to_ipc()`` gives the same bytes in memory; read either back with
``pa.ipc.open_stream``. For the IPC file format, convert through pyarrow
(``pa.ipc.new_file(...).write_table(df.to_arrow())``).

The Arrow C Data Interface (C++)
--------------------------------

The zero-copy bridge is the free functions ``to_arrow``, ``series_from_arrow``
and ``dataframe_from_arrow`` in the installed public headers
``dftracer/utils/arrow/arrow.h`` and ``dftracer/utils/arrow/batch.h``
(namespace ``dftracer::utils::arrow``, library ``dftracer_utils_arrow``, which
only Python and tests link). ``to_arrow`` returns a ``Batch`` - a move-only
RAII owner of the paired ``ArrowSchema`` / ``ArrowArray`` whose buffers alias
the column's - so the caller never juggles release callbacks by hand. A
``DataFrame`` exports as a struct array with one child per column. Every call
reports failure as a ``dstd::Result``. The headers pull in the Arrow C Data
Interface struct definitions themselves, so a consumer needs no Arrow library
to include them.

.. code-block:: cpp

   #include <dftracer/utils/arrow/arrow.h>

   using namespace dftracer::utils;

   dstd::Result<arrow::Batch> a = arrow::to_arrow(df);   // zero-copy export
   if (!a) return a.error();
   // ... hand (a->schema(), a->array()) to any Arrow consumer, or give the
   // structs away with a->release_into(&schema, &array) ...

   // A single column round-trips the same way.
   dstd::Result<arrow::Batch> col = arrow::to_arrow(df.column("dur"));
   dstd::Result<dataframe::Series> s =
       arrow::series_from_arrow(std::move(*col));

The IPC format is ``arrow::Writer`` and ``arrow::Reader`` in
``dftracer/utils/arrow/ipc.h``. A writer goes to a file or to a buffer, takes
one ``DataFrame`` per record batch and is closed by ``finish()``. A reader
hands back one record batch per ``next()`` as a ``DataFrame``. ``scan_ipc``
returns a ``LazyFrame`` over the record batches of several files, read in
parallel by the engine and honouring the column projection, and ``read_ipc``
is that scan collected.

.. code-block:: cpp

   #include <dftracer/utils/arrow/ipc.h>

   arrow::Writer writer = arrow::Writer::to_buffer();
   writer.write(df);
   writer.finish();
   dstd::Vector<std::uint8_t> bytes = writer.take_bytes();

   dstd::Result<arrow::Reader> reader =
       arrow::Reader::from_bytes(dstd::Span<const std::uint8_t>(
           bytes.data(), bytes.size()));     // the bytes must outlive it
   while (auto batch = reader->next()) {
       if (!batch->has_value()) break;
       dataframe::DataFrame part = std::move(**batch);
   }

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
