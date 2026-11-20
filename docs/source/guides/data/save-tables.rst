:description: Store a pandas frame as a typed DFTracer JSON-lines table file with write_table and read_table, and read it back with the same dtypes and index.

Save a frame as a table file
============================

.. admonition:: Goal
   :class: goal

   Keep a pandas frame, such as an analysis checkpoint, in a file that reads
   back with the same dtypes and index and that ``zcat`` and ``jq`` can read.

Write and read
--------------

.. code-block:: python

   import numpy as np
   import pandas as pd
   from dftracer.utils import read_table, write_table

   # pd.array([0.5, nan, None]) would turn the NaN into NA; the mask keeps both.
   time = pd.arrays.FloatingArray(np.array([0.5, np.nan, 0.0]), np.array([False, False, True]))
   df = pd.DataFrame(
       {
           "calls": pd.array([3, None, 7], dtype="Int64"),
           "time": time,
           "kind": pd.Categorical(["read", "write", None], ordered=False),
       },
       index=pd.Index(["a", "b", "c"], name="proc"),
   )
   write_table(df, "profile.pfw.gz", name="profile", meta={"run": 1})
   same = read_table("profile.pfw.gz")

A path ending in ``.gz`` is a multi-member gzip file, which ``read_table``
decodes in parallel. NA and NaN stay apart in the ``Float64`` column, and the
index comes back with its name.

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
partition, and ``read_table`` of that folder reads them back in name order.

.. code-block:: python

   import dask.dataframe as dd

   write_table(dd.from_pandas(df, npartitions=4), "parts", name="profile")
   whole = read_table("parts")
