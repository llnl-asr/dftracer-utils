:description: Reference for write_table, read_table and TableWriter, which store a pandas frame as a typed DFTracer gzip JSON-lines table file.

Tables (write_table, read_table)
================================

``write_table`` stores a pandas frame as one DFTracer-style file of JSON lines,
and ``read_table`` reads it back with the same dtypes and index. The file is
gzip when the path ends in ``.gz``, readable with ``zcat`` and ``jq``, and its
members decode in parallel. Encoding and decoding run in C++.

.. code-block:: python

   import pandas as pd
   from dftracer.utils import read_table, write_table

   df = pd.DataFrame({"a": pd.array([1, None, 3], dtype="Int64"), "s": ["x", "y", None]})
   write_table(df, "frame.pfw.gz", name="frame", meta={"source": "demo"})
   back = read_table("frame.pfw.gz")

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
``StringDtype("pyarrow")``, which print alike. Every other line is a ``ph`` 3 record whose ``args``
hold one row, in field order.

NA is ``null``. NaN, ``+inf`` and ``-inf`` are the strings ``"NaN"``,
``"Infinity"`` and ``"-Infinity"``, so a nullable float column keeps NaN apart
from NA. Other floats are the shortest text that reads back to the same bits. A
category cell is its label, and a set or list cell is a JSON array.

A default ``RangeIndex`` is not stored. Any other index becomes leading fields,
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
