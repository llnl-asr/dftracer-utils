:description: Reference for the DataFrame Python wrapper: build, project, filter, reshape, and join typed columns bridged zero-copy to Arrow and pandas.

DataFrame
=========

``DataFrame`` is the Python wrapper over the native columnar frame: a named,
ordered set of typed :doc:`Series <series>`, computed with SIMD kernels and
bridged zero-copy to Arrow, pandas, NumPy, and polars only at the edge. It is
what :class:`~dftracer.utils.TraceViewer` terminals (``collect``,
``collect_typed``, ``merge_partials``) return, and what the constructors below
build from other sources.

.. code-block:: python

   from dftracer.utils import DataFrame, F

   df = DataFrame.from_pandas(pdf)
   df = DataFrame.from_arrow(table)

   avg = df.apply(F.sum_dur / F.count)     # frame-first spelling -> Series
   top = df.sort_by("sum_dur", descending=True).head(10)

For the full operation catalog (project, filter, reshape, join, windowed
aggregation) with C++/Python tabs, see :doc:`../guides/data/dataframe` and
:doc:`../guides/core/columnar-ops`. This page is the generated reference.

Type relationships
------------------

How the DataFrame wrapper relates to Series and its native base:

.. mermaid:: /_generated/py_dataframe.mmd

.. autoclass:: dftracer.utils.DataFrame
   :members:
   :undoc-members:
   :show-inheritance:

``DataFrame.apply``
--------------------

``df.apply(expr)`` is the frame-first spelling of evaluating a
:class:`~dftracer.utils.columnar.ColumnExpr`, equivalent to
``expr.apply(df)``:

.. code-block:: python

   from dftracer.utils import F

   avg = df.apply(F.sum_dur / F.count)          # frame-first
   avg = (F.sum_dur / F.count).apply(df)         # expression-first, identical result

Use whichever reads better at the call site; see :doc:`columnar` for the
expression DSL itself.

See also
--------

- :doc:`series` for the column type ``DataFrame`` is built from.
- :doc:`columnar` for the expression DSL evaluated by ``.apply()``.
- :doc:`trace_viewer` for ``TraceViewer``, the query API that produces a
  ``DataFrame``.
- :doc:`../guides/data/dataframe` for a task-oriented walkthrough.
