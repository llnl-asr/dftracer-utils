:description: Store a DataFrame as a typed DFTracer JSON-lines table file with DataFrame.write_table, read it back with TraceViewer, and get the exact pandas frame with to_pandas().

Save a frame as a table file
============================

.. admonition:: Goal
   :class: goal

   Keep a frame, such as an analysis checkpoint, in a file that reads back
   with the same types and index and that ``zcat`` and ``jq`` can read.

Write and read
--------------

.. code-block:: python

   from dftracer.utils import DataFrame
   from dftracer.utils.trace import TraceViewer

   df = DataFrame.from_dict(
       {
           "proc": ["a", "b", "a", "c"],
           "calls": [3, 5, 7, 2],
           "time": [0.5, 1.5, 2.0, 0.25],
       }
   )
   df.write_table("profile.pfw.gz", name="profile", meta={"run": 1}, mode="overwrite")
   profile = TraceViewer("profile.pfw.gz").collect()

A path ending in ``.pfw.gz`` is a multi-member gzip file, which a read decodes
in parallel; ``.pfw`` is plain text; any other path is a folder of
``part-NNNNN.pfw.gz`` files. The write is staged beside the target and renamed
into place, so a failed write leaves nothing. To filter a table before it is in
memory, or to read it from its index, use
:class:`~dftracer.utils.trace.TraceViewer` itself (see :doc:`/api/trace_viewer`).
``collect()`` returns a dftracer.utils ``DataFrame``, so the rest of the engine
works on it directly:

.. code-block:: python

   busy = profile.filter(profile["calls"] > 2)
   per_proc = profile.group_by("proc").sum()

A List column is stored as the JSON array of each cell, with the type of its
items in the header. ``collect()`` gives a List column of that item type (a
list of ``int32`` has an Int32 child), and ``to_pandas()`` gives the lists
back. A list element must be a string, a number, a bool or null; a NaN or
infinite float element is refused. A table file with no item type in its header
reads the cells as JSON text.

Get the exact pandas frame
--------------------------

The frame a table read returns carries its table header. ``to_pandas()`` with
no arguments uses it to give the pandas frame that was written, with its
dtypes, categories and index. NA and NaN stay apart in a nullable float column.

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
   DataFrame.from_pandas(original).write_table("typed.pfw.gz", name="typed", mode="overwrite")
   exact = TraceViewer("typed.pfw.gz").collect().to_pandas()

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

Write a plan without collecting it
----------------------------------

``LazyFrame.sink_table`` and ``TraceViewer.sink_table`` write a plan as it
streams, in parts of ``part_size`` rows when you ask for them. Pass
``lazy=True`` to get a :class:`~dftracer.utils.LazyResult` for
:func:`~dftracer.utils.collect_all`. To write events instead, use
``write_trace``, ``sink_trace`` on a plan that keeps the event envelope.

.. code-block:: python

   from dftracer.utils import col

   summary = profile.lazy().filter(col("calls") > 2).sink_table(
       "busy", part_size=2, mode="overwrite"
   )
   whole = TraceViewer("busy").collect()
