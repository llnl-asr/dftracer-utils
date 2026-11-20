:description: Reference for write_table, read_table and TableWriter, which store a DataFrame as a typed DFTracer gzip JSON-lines table file and read it back.

Tables (write_table, read_table)
================================

``write_table`` stores a dftracer.utils ``DataFrame`` (or a pandas frame) as one
DFTracer-style file of JSON lines, and ``read_table`` reads it back as a
dftracer.utils ``DataFrame``. The file is gzip when the path ends in ``.gz``,
readable with ``zcat`` and ``jq``, and its members decode in parallel. Encoding
and decoding run in C++, and ``read_table`` builds no pandas frame.

.. code-block:: python

   from dftracer.utils import DataFrame, read_table, write_table

   df = DataFrame.from_dict(
       {"proc": ["a", "b", "a"], "calls": [3, 5, 7], "time": [0.5, 1.5, 2.0]}
   )
   write_table(df, "frame.pfw.gz", name="frame", meta={"source": "demo"})
   frame = read_table("frame.pfw.gz")
   busy = frame.filter(frame["calls"] > 3)
   totals = frame.group_by("proc").sum()

``frame.to_pandas()`` with no arguments gives the exact pandas frame that was
written, with its dtypes, categories, set and list cells and index, because the
frame ``read_table`` returns carries the table header:

.. code-block:: python

   import pandas as pd

   original = pd.DataFrame(
       {"a": pd.array([1, None], dtype="Int64"), "s": pd.array(["x", None], dtype="string[python]")}
   )
   write_table(original, "typed.pfw.gz", name="typed")
   exact = read_table("typed.pfw.gz").to_pandas()

A new frame (a filter, a join) has no header, and a frame changed in place
(``df["a"] = ...``, ``df.loc[...] = ...``) drops it. ``to_pandas`` with an explicit ``arrow``, ``nullable`` or ``index``
argument ignores the header too. Each of these behaves as ``to_pandas`` does
for any frame.

Columns of the frame ``read_table`` returns
-------------------------------------------

The columns are the engine's types: Int64, Uint64, Float64, Bool and String,
with a null for NA (a float NaN stays a value). A ``category`` column holds its
labels as plain values (the native column is not dictionary-encoded), and a set
or list column holds the JSON text of each cell, because the engine has no list
type; ``to_pandas()`` restores the categories and builds the sets and lists. The
header's index levels are the frame's row index, so ``frame.index_names()``
lists them.

Writing a dftracer.utils frame
------------------------------

A dftracer.utils frame is written from its columns, with no pandas step. A
column keeps its width (``int32``, ``uint8``, ``float32``); a column that holds
a null is written with a nullable dtype (``Int32``, ``Float64``, ``boolean``),
so a null stays apart from NaN; a string column is written as ``object`` and a
dictionary-encoded string column as a ``category``. The frame's row index names
become the header's index. A column of another Arrow type (timestamps, lists,
structs) is refused with ``TypeError``.

File format
-----------

Line 1 is a ``ph`` 4 record. Its ``args`` hold ``format`` (``"dftu.table"``),
``format_version`` (1), ``table``, ``nrows``, ``index`` (the index levels, as
fields), ``index_names``, ``fields`` and ``meta``. Each field has a ``name``
and the exact pandas ``dtype``; a ``category`` field adds ``categories`` and
``ordered`` and ``categories_dtype`` (the dtype of the categories, restored
exactly, such as ``int32`` or ``float32``), and an ``object`` field of sets or
lists adds ``elem`` (``"set"`` or ``"list"``). A ``StringDtype`` with NA names
its storage, as ``string[pyarrow]`` or ``string[python]``. An ``ArrowDtype``
field adds ``arrow_type``, the text of its pyarrow type (``"string"``,
``"int64"``, ``"double"``), which the reader rebuilds with
``pyarrow.type_for_alias``; this keeps ``ArrowDtype(pa.string())`` apart from
``StringDtype("pyarrow")``, which print alike. Every other line is a ``ph`` 3
record whose ``args`` hold one row, in field order.

NA is ``null``. NaN, ``+inf`` and ``-inf`` are the strings ``"NaN"``,
``"Infinity"`` and ``"-Infinity"``, so a nullable float column keeps NaN apart
from NA. Other floats are the shortest text that reads back to the same bits. A
category cell is its label, and a set or list cell is a JSON array.

For a pandas frame a default ``RangeIndex`` is not stored. Any other index becomes leading fields,
and an unnamed level is stored as ``__index_level_<i>__`` and restored without
a name. DFAnalyzer's checkpoint tables use the same format, and each side reads
the other's files.

What a table carries
--------------------

Carried: ``int8`` to ``int64``, ``uint8`` to ``uint64``, ``float32``,
``float64``, ``bool``, the nullable ``Int*``, ``UInt*``, ``Float32``,
``Float64`` and ``boolean``, ``string``, Arrow-backed dtypes such as
``string[pyarrow]`` and ``int64[pyarrow]``, ``object`` columns of ``str`` cells
(or sets or lists of scalars), and ``category`` with all-string or all-numeric
labels. Refused with ``TypeError``: datetime, timedelta, period and interval
columns, an object column of anything else, and a NaN or infinity inside a set
or list cell, boolean categories, and an ``ArrowDtype`` whose type
``pyarrow.type_for_alias`` cannot rebuild. Refused with ``ValueError``: duplicate
column names, index level names that collide with column names, category labels
above ``2**63 - 1`` or infinite, and a column that mixes sets and lists. An
index level name that is not a ``str`` is stored as its text and restored as
given.

Folders and Dask
----------------

``read_table`` of a folder reads every ``*.pfw.gz`` in it in name order and
concatenates them; the parts must agree on their fields and index.
``write_table`` of a Dask ``DataFrame`` writes such a folder, one file per
partition (``part-00000.pfw.gz`` and on), each with its own header.

Functions
---------

.. autofunction:: dftracer.utils.write_table

.. autofunction:: dftracer.utils.read_table

.. autoclass:: dftracer.utils.TableWriter
   :members:
   :special-members: __enter__, __exit__
