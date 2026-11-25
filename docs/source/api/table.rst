.. meta::
   :description: Reference for DataFrame.write_table and TraceViewer, which store a DataFrame as a typed DFTracer table file and read it back.

Tables (DataFrame.write_table, TraceViewer)
===========================================

``DataFrame.write_table`` stores a dftracer.utils ``DataFrame`` as DFTracer-style
files of JSON lines, and ``TraceViewer(path).collect()`` reads them back as a
dftracer.utils ``DataFrame``. A path ending in ``.pfw.gz`` is one gzip file,
readable with ``zcat`` and ``jq``, whose members decode in parallel; ``.pfw``
is plain text; any other path is a folder of ``part-NNNNN.pfw.gz`` files.
Encoding and decoding run in C++.

.. code-block:: python

   from dftracer.utils import DataFrame
   from dftracer.utils.trace import TraceViewer

   df = DataFrame.from_dict(
       {"proc": ["a", "b", "a"], "calls": [3, 5, 7], "time": [0.5, 1.5, 2.0]}
   )
   summary = df.write_table("frame.pfw.gz", name="frame", meta={"source": "demo"}, mode="overwrite")
   frame = TraceViewer("frame.pfw.gz").collect()
   busy = frame.filter(frame["calls"] > 3)
   totals = frame.group_by("proc").sum()

``frame.to_pandas()`` with no arguments gives the exact pandas frame that was
written, with its dtypes, categories and index, because the frame a table read
returns carries the table header, and ``write_table`` stores a header that keeps it.

.. code-block:: python

   import pandas as pd

   original = DataFrame.from_pandas(
       pd.DataFrame({"a": pd.array([1, None], dtype="Int64"), "s": ["x", None]})
   )
   original.write_table("typed.pfw.gz", name="typed", mode="overwrite")
   exact = TraceViewer("typed.pfw.gz").collect().to_pandas()

A table can sit in the same folder as traces, such as a checkpoint written next to the traces it came from.
When the files given to ``TraceViewer``, the ``Indexer`` or ``discover_trace_files`` hold both traces and tables, the tables are left out and only the traces are read.
A file is a table when its first line is a dftu.table header record.
When every file is a table, they are read as one table.

A new frame (a filter, a join) has no header, and a frame changed in place
(``df["a"] = ...``, ``df.loc[...] = ...``) drops it. ``to_pandas`` with an explicit ``arrow``, ``nullable`` or ``index``
argument ignores the header too. Each of these behaves as ``to_pandas`` does
for any frame.

Writing
-------

The verb chooses what is written. ``write_table`` writes typed records: line 1
is the header and every other line is one row. ``LazyFrame.sink_table`` and
``TraceViewer.sink_table`` write a plan the same way without collecting it.
``write_trace``, ``LazyFrame.sink_trace`` and ``TraceViewer.sink_trace`` write
DFTracer events and need the event envelope (``name``, ``cat``, ``pid``,
``tid``, ``ts``, ``ph``, with ``dur`` and ``args.<key>`` columns); a plan
without it raises an error that names the missing columns. A
``TraceViewer.sink_trace`` of the viewer's own events (no column op, select,
row window or ``partition_by``) writes each event line as the trace holds it,
so a key that no column carries stays. The sink verbs take
``lazy=True`` to return a :class:`~dftracer.utils.LazyResult` for
:func:`~dftracer.utils.collect_all`; the write verbs run at once. ``name`` and
``meta`` set the table name and metadata, so only the table verbs take them. A
plugin format has the generic ``sink(path, format=...)`` on a plan and
``write(path, format=...)`` on a frame; ``format`` is required. The output is staged next to the target
(``.<name>.tmp-<id>``), synced and renamed into place, so a failed write leaves
nothing at the target; ``mode="error"`` refuses an existing target and
``mode="overwrite"`` replaces it. A leftover staging path of a crashed write
is removed by the next write of that target. ``part_size`` is the rows of one
part of a folder. One index is built for all the files, after the rename; a
crash between the two leaves data that readers scan. ``TraceWriteOptions``
sets ``level``, ``member_size`` (0 is automatic, 4 MiB of text per gzip member)
and ``build_index``.

A streamed plan cannot know before it runs whether a column holds a null or how
many rows it has. The header of a ``sink_table`` therefore types integers
and booleans as the nullable pandas dtypes and starts with ``nrows`` ``-1`` in a
20-character slot, which the writer overwrites with the part's row count (and
the CRC32 of the header member) before it syncs. A reader refuses a file whose
slot still holds ``-1``. ``DataFrame.write_table`` knows both and writes them at once.

Columns of the frame a table read returns
-----------------------------------------

The columns are the engine's types: Int64, Uint64, Float64, Bool and String,
with a null for NA (a float NaN stays a value). A ``category`` column holds its
labels as plain values (the native column is not dictionary-encoded), and a set
or list column holds the JSON text of each cell, because the engine has no list
type; ``to_pandas()`` restores the categories and builds the sets and lists.
``to_pandas(set_type=...)`` builds each set cell with another type, such as
``frozenset`` or a ``set`` subclass, from the list of its values. The
header's index levels are the frame's row index, so ``frame.index_names()``
lists them.

Writing a dftracer.utils frame
------------------------------

A dftracer.utils frame is written from its columns. A
column keeps its width (``int32``, ``uint8``, ``float32``); a column that holds
a null is written with a nullable dtype (``Int32``, ``Float64``, ``boolean``),
so a null stays apart from NaN; a string column is written as ``object`` and a
dictionary-encoded string column as a ``category``. The frame's row index names
become the header's index. A column of another type (timestamps, lists,
structs) is refused with ``TypeError``.

File format
-----------

Line 1 is a ``ph`` 4 record. Its ``args`` hold ``format`` (``"dftu.table"``),
``format_version`` (the ``table`` line of ``VERSION``), ``table``, ``nrows``, ``index`` (the index levels, as
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

``format_version`` is the string ``"MAJOR.MINOR"`` (the ``table`` line of
``VERSION``, ``"1.0"`` today). A new optional header key raises the minor; a
change that an old reader cannot read raises the major. A reader takes a file
whose major equals its own and whose minor is not greater, and refuses any other
value, a malformed string or an unknown major, naming the file's value and its
own version. The integer ``1``, as DFAnalyzer's writer stamps it, reads as
``"1.0"``.

NA is ``null``. NaN, ``+inf`` and ``-inf`` are the strings ``"NaN"``,
``"Infinity"`` and ``"-Infinity"``, so a nullable float column keeps NaN apart
from NA. Other floats are the shortest text that reads back to the same bits.
An infinite float category label is written the same way. A category cell is
its label, and a set or list cell is a JSON array.

``DataFrame.from_pandas`` turns a pandas index into columns and the frame's row
index; an unnamed level is stored as ``__index_level_<i>__`` and restored
without a name. DFAnalyzer's checkpoint tables use the same format, and each side reads
the other's files.

What a table carries
--------------------

Carried: ``int8`` to ``int64``, ``uint8`` to ``uint64``, ``float32``,
``float64``, ``bool``, the nullable ``Int*``, ``UInt*``, ``Float32``,
``Float64`` and ``boolean``, strings, and dictionary-encoded string columns as
``category``. A column of another type (timestamps, lists, structs) is
refused with ``TypeError``. A table file written by another tool may also hold
Arrow-backed dtypes, set and list columns and categories of numbers; a table
read restores them and keeps them when the frame is written again.

Folders
-------

A table read of a folder reads every ``*.pfw.gz`` in it in part-number order
and concatenates them; the parts must agree on their fields and index.
``write_table`` with ``part_size`` writes such a folder. A folder is always gzip:
``TraceWriteOptions(compress=False)`` for a folder raises, and plain output is
one ``.pfw`` file.

Classes
-------

.. autoclass:: dftracer.utils.SinkSummary

.. autoclass:: dftracer.utils.TraceWriteOptions

.. automethod:: dftracer.utils.DataFrame.write_table

.. automethod:: dftracer.utils.DataFrame.write_trace

.. automethod:: dftracer.utils.DataFrame.write

.. automethod:: dftracer.utils.LazyFrame.sink_table

.. automethod:: dftracer.utils.LazyFrame.sink_trace

.. automethod:: dftracer.utils.LazyFrame.sink
