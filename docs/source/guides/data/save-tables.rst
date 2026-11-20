:description: Store a DataFrame as a typed DFTracer JSON-lines table file with write_table, read it back with read_table, and get the exact pandas frame with to_pandas().

Save a frame as a table file
============================

.. admonition:: Goal
   :class: goal

   Keep a frame, such as an analysis checkpoint, in a file that reads back
   with the same types and index and that ``zcat`` and ``jq`` can read.

Write and read
--------------

.. code-block:: python

   from dftracer.utils import DataFrame, read_table, write_table

   df = DataFrame.from_dict(
       {
           "proc": ["a", "b", "a", "c"],
           "calls": [3, 5, 7, 2],
           "time": [0.5, 1.5, 2.0, 0.25],
       }
   )
   write_table(df, "profile.pfw.gz", name="profile", meta={"run": 1})
   profile = read_table("profile.pfw.gz")

A path ending in ``.gz`` is a multi-member gzip file, which ``read_table``
decodes in parallel. ``read_table(path)`` is ``TraceViewer(path).collect()``:
to filter a table before it is in memory, or to read it from an index, use
:class:`~dftracer.utils.trace.TraceViewer` itself (see :doc:`/api/trace_viewer`). ``read_table`` returns a dftracer.utils ``DataFrame``, so
the rest of the engine works on it directly:

.. code-block:: python

   busy = profile.filter(profile["calls"] > 2)
   per_proc = profile.group_by("proc").sum()

Get the exact pandas frame
--------------------------

The frame ``read_table`` returns carries its table header. ``to_pandas()`` with
no arguments uses it to give the pandas frame that was written, with its
dtypes, categories, set and list cells and index. NA and NaN stay apart in a
nullable float column.

.. code-block:: python

   import numpy as np
   import pandas as pd

   # pd.array([0.5, nan, None]) would turn the NaN into NA; the mask keeps both.
   time = pd.arrays.FloatingArray(np.array([0.5, np.nan, 0.0]), np.array([False, False, True]))
   original = pd.DataFrame(
       {
           "calls": pd.array([3, None, 7], dtype="Int64"),
           "time": time,
           "kind": pd.Categorical(["read", "write", None], ordered=False),
       },
       index=pd.Index(["a", "b", "c"], name="proc"),
   )
   write_table(original, "typed.pfw.gz", name="typed")
   exact = read_table("typed.pfw.gz").to_pandas()

A new frame (a filter, a join) has no header, and a frame changed in place
(``df["a"] = ...``, ``df.loc[...] = ...``) drops it. ``to_pandas`` with an explicit ``arrow``, ``nullable`` or ``index``
argument ignores the header too. Each of these gives what ``to_pandas`` gives
any frame.

Look at the file
----------------

.. code-block:: bash

   zcat profile.pfw.gz | head -n 2 | jq -c .

Line 1 is the header with the dtype of every field. Each other line is one row
in ``args``. See :doc:`../../api/table` for the format and what a table cannot
carry.

Append frames
-------------

``TableWriter`` takes frames of one schema and writes one file when the block
ends. It holds the frames in memory until then, because line 1 states the total
row count.

.. code-block:: python

   from dftracer.utils import TableWriter

   with TableWriter("chunks.pfw.gz", name="chunks") as w:
       for part in (df, df):
           w.write(part)

A block that ends by an exception writes nothing.

Dask
----

``write_table`` of a Dask ``DataFrame`` writes a folder with one table file per
partition, and ``read_table`` of that folder reads them back in part-number order.

.. code-block:: python

   import dask.dataframe as dd

   write_table(dd.from_pandas(original, npartitions=2), "parts", name="profile")
   whole = read_table("parts")
