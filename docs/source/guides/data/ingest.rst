:description: Import from pandas, polars, Arrow, Parquet, dicts, or NumPy and hand data back out, crossing the Arrow C Data Interface without copying.

Ingest and interoperate with other data tools
================================================

``DataFrame`` and ``Series`` are not an island: every ``from_*`` classmethod
imports another library's data, and every ``to_*`` method hands it back out,
so you can keep the rest of your analysis in pandas, polars, or plain NumPy
while dftracer-utils does the columnar work. Most of these paths cross the
boundary through the `Arrow C Data Interface
<https://arrow.apache.org/docs/format/CDataInterface.html>`_ - a shared memory
layout both sides read directly - so importing a ``pyarrow.Table`` or a
``polars.DataFrame`` does not copy the underlying buffers.

Bring data in
-------------

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import DataFrame, Series

         df = DataFrame.from_pandas(pandas_df)
         df = DataFrame.from_arrow(pyarrow_table)
         df = DataFrame.from_polars(polars_df)
         df = DataFrame.from_parquet("data.parquet", columns=["dur", "cat"])
         df = DataFrame.from_dict({"cat": ["POSIX", "STDIO"], "dur": [100, 200]})
         df = DataFrame.from_numpy(arr, columns=["a", "b"])  # 2-D array, or {name: 1-D array}

         s = Series.from_pandas(pandas_series)
         s = Series.from_arrow(pyarrow_array)
         s = Series.from_polars(polars_series)
         s = Series.from_numpy(numpy_array)
         s = Series.from_list([1, 2, 3])            # optional dtype= a native DType or a pyarrow type

Only the Arrow paths need pyarrow. Everything else reads through the native
engine, so a build without Arrow support and an environment without pyarrow
still import and export every column type:

- ``Series.from_arrow`` accepts any object implementing the Arrow PyCapsule
  protocol (``__arrow_c_array__``) and imports it with no copy.
  ``DataFrame.from_arrow`` takes a ``pyarrow.Table`` or ``RecordBatch`` (or
  anything exposing the same ``column_names`` / ``column()`` duck-typed API)
  and imports each column the same zero-copy way.
- ``from_pandas`` and ``from_polars`` read through NumPy: NumPy-backed and
  nullable pandas dtypes, categoricals, zoned ``datetime64`` and ``timedelta64``
  columns, and polars numeric, boolean, text, binary, date, datetime,
  duration, time, decimal and categorical columns. A float ``NaN`` in pandas,
  a ``NaT`` and a missing value read as null; a non-default pandas index
  becomes columns after the data columns. Polars list, array and struct
  columns import as list and struct columns. A pandas Arrow-backed column
  needs pyarrow (pandas itself needs it to hold one).
- ``from_parquet`` reads through ``pyarrow.parquet``.
- ``from_dict`` builds the frame natively from Series, lists of scalars, NumPy
  arrays and pandas or polars Series; a mapping with anything else (a pyarrow
  array, nested lists) goes through ``pyarrow.table``.
  A list with no value in it (empty, or only ``None``) and no ``dtype`` is a
  ``string`` column of nulls, in ``from_dict``, the ``DataFrame`` constructor and
  ``Series.from_list``; that is the type ``from_pandas`` gives a column of
  ``None``. ``astype`` turns it into another type, or ``Series.from_list(values,
  dtype=...)`` types it up front.
- ``Series.from_numpy`` borrows the buffer of a 1-D, C-contiguous, fixed-width
  numeric array (no copy, no pyarrow). Bool, ``float16``, ``datetime64``,
  ``timedelta64``, text, bytes and object arrays, non-contiguous arrays and
  masked arrays are copied natively. ``DataFrame.from_numpy`` builds each
  column through ``Series.from_numpy``.
- ``Series.from_list`` has no zero-copy shortcut, since a Python list has no
  buffer to share. It reads ``bool``, ``int`` (``uint64`` when too large),
  ``float``, ``str``, ``bytes``, ``datetime.datetime`` (naive or aware, in
  microseconds; the zone is the ``zoneinfo`` key, ``UTC`` or a fixed offset),
  ``datetime.date``, ``datetime.time``, ``datetime.timedelta`` and
  ``decimal.Decimal`` (decimal128 with the widest scale, up to 38 digits), with
  ``None`` for null, natively. Lists and tuples become list columns and dicts struct
  columns (keys in first-seen order, a missing key null), to any depth. Pass a
  native ``DType`` as ``dtype=`` to type an empty or all-null list; a pyarrow
  type goes through pyarrow.

Every ``from_*`` path except ``from_arrow`` works without pyarrow, apart from
the cases above; ``from_arrow`` itself needs only the capsule protocol.

Get data out
------------

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         df.to_arrow()      # pyarrow.Table, zero-copy
         df.to_pandas()      # pandas.DataFrame
         df.to_polars()      # polars.DataFrame (needs polars installed)

         s.to_arrow()        # pyarrow.Array, zero-copy
         s.to_pandas()        # pandas.Series
         s.to_numpy()          # NumPy array
         s.to_polars()          # polars.Series (needs polars installed)

``to_arrow()`` is the zero-copy exit on both classes and needs pyarrow.
``to_pandas()``, ``to_polars()``, ``to_numpy()`` and ``to_list()`` are built
natively, with no pyarrow, for every column type: a flat, non-null,
fixed-width numeric column is read straight from the native buffer; nulls in
an integer column become NaN (float64); dates and timestamps are
``datetime64`` (UTC values for a zoned timestamp, which pandas and polars
show in their zone), durations ``timedelta64``, with ``NaT`` for null; text,
bytes, decimals, times, lists, structs and maps are object arrays of Python
objects. ``to_list()`` gives ``datetime``, ``date``, ``time``, ``timedelta``
and ``decimal.Decimal`` objects (nanoseconds truncate to microseconds there
only). ``to_polars()`` reads the native Arrow stream through polars' Arrow import
(polars 1.3 or later, no pyarrow). ``to_pandas()`` builds a string column from
its Arrow array when pandas keeps strings in Arrow (pandas 3 with pyarrow).
Float16, decimal and dictionary columns keep the native conversion, which gives
them a different dtype than Arrow does. ``to_pandas(arrow=True)`` keeps the Arrow-backed dtypes and needs
pyarrow. ``df.to_numpy()`` gives the 2-D array of the whole frame, and
``df["dur"].to_numpy()`` gives one column. A ``Series`` and a ``DataFrame`` pickle in the native frame format.

``np.asarray(series)`` also works directly (``Series`` implements the NumPy
array protocol), and Series arithmetic (``+ - * /``) and comparisons against
another ``Series`` or a Python scalar return a ``Series``, so you rarely need
to round-trip through NumPy just to do elementwise math.

A worked round trip
--------------------

.. code-block:: python

   import pandas as pd
   from dftracer.utils import Series, TraceViewer

   result = (
       TraceViewer("traces/")
       .group_by("cat")
       .agg("count", "mean:dur")
       .collect()
   )

   # Do the rest of the analysis in pandas:
   pdf = result.to_pandas()
   pdf["mean_dur"].plot.bar()

   # Or hand a NumPy array straight to something that only wants numbers:
   import numpy as np
   durs = result["mean_dur"].to_numpy()
   np.percentile(durs, 90)

   # Bring an externally-computed column back in:
   adjusted = pd.Series(durs * 1.1, name="adjusted_mean_dur")
   result_with_adjustment = result.with_column(
       "adjusted_mean_dur", Series.from_pandas(adjusted)
   )

The C++ engine and Arrow
--------------------------

The C++ ``dataframe::Series``/``dataframe::DataFrame`` API exposes a zero-copy
Arrow bridge through the installed public header
``dftracer/utils/dataframe/arrow.h``: ``Series::to_arrow`` /
``DataFrame::to_arrow`` return an ``OwnedArrow`` (a move-only owner of the
paired ``ArrowSchema`` / ``ArrowArray``), and ``Series::from_arrow(schema,
array)`` / ``DataFrame::from_arrow(schema, array)`` import back. A
``DataFrame`` is represented as a struct array with one child per column. The
bridge sits on the same C ABI a C caller would use: the flat-column functions
(``dftu_series_new_flat``, ``dftu_series_data``, and the Arrow-layout validity
bitmap they take) already speak Arrow's physical layout, so a C or C++ program
importing from another Arrow-based library builds columns through that
boundary - see :doc:`../core/c-abi`.

See also
--------

- :doc:`dataframe` and :doc:`series` for the rest of the ``DataFrame``/``Series``
  API once your data is in.
- :doc:`../core/c-abi` for the C ABI's own zero-copy column boundary.
