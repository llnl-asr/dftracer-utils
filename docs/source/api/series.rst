:description: Reference for the Series Python wrapper: SIMD ops and NumPy-style arithmetic over one typed native column, converted to Arrow or pandas at the edge.

Series
======

``Series`` is the Python wrapper over one typed, native column: SIMD ops from
the DataFrame engine plus NumPy-style ``+ - * /`` and ``s[i]`` / ``s[a:b]``
indexing, the ``np.asarray`` protocol, and Arrow/pandas/NumPy/polars
conversion at the edge. It is the column counterpart of
:doc:`DataFrame <dataframe>` - ``df["col"]`` returns one.

.. code-block:: python

   from dftracer.utils import Series

   s = Series.from_numpy(arr)
   s2 = s * 2 + 1
   p99 = s.quantile(0.99)
   arr = s.to_numpy()

For the full operation catalog (reducers, element-wise kernels, scan/window
ops, string ops) with C++/Python tabs, see :doc:`../guides/data/series` and
:doc:`../guides/core/columnar-ops`. This page is the generated reference.

Type relationships
------------------

How Series relates to its wrapper base:

.. mermaid:: /_generated/py_series.mmd

.. autoclass:: dftracer.utils.Series
   :members:
   :undoc-members:
   :show-inheritance:

Comparisons (``<``, ``<=``, ``>``, ``>=``, ``==``, ``!=``, plus the named
``gt``/``ge``/``lt``/``le``/``eq``/``ne`` methods) return a boolean mask
``Series``, as in pandas and polars. A ``Series`` is therefore unhashable. The
native C++ ``Series`` has no ``operator==``/``!=``, so use ``eq()`` / ``ne()``
there.

See also
--------

- :doc:`dataframe` for the frame type built from ``Series`` columns.
- :doc:`columnar` for the expression DSL that produces derived ``Series``.
- :doc:`../guides/data/series` for a task-oriented walkthrough.
