:description: Change a frame's shape without changing its values: pivot long to wide, explode a list column into rows, or one-hot encode a categorical column.

Reshape frames
==============

Change a frame's shape without changing its values: pivot long rows into wide
columns (and back), expand a list column into rows, or one-hot encode a
categorical column. Every op returns a new frame.

Pivot long to wide
-------------------

``pivot`` turns the distinct values of one column into columns of their own. Rows
become the distinct ``index`` values (sorted); there is one value column per
distinct pivot-column value (sorted, named by its value); each cell is the
``values`` column aggregated over the matching rows.

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/dataframe/dataframe.h>

         using namespace dftracer::utils::dataframe;

         DataFrame wide = df.pivot("ts", "cat", "dur", Agg::Sum);

   .. tab-item:: Python

      .. code-block:: python

         wide = df.pivot(index="ts", columns="cat", values="dur", agg="sum")

   .. tab-item:: C

      .. code-block:: c

         #include <dftracer/utils/dataframe/abi.h>

         dftu_dataframe* wide = dftu_dataframe_pivot(df, "ts", "cat", "dur", "sum");

The collision reducer ``agg`` is one of ``first`` / ``last`` / ``sum`` / ``min``
/ ``max`` / ``mean`` (``mean`` yields ``Float64``); it defaults to ``first``. In
C++ ``pivot`` also accepts an ``Agg`` enumerator (``Agg::Sum``, ``Agg::First``,
...) in place of the string.

Unpivot wide to long
--------------------

``unpivot`` (aliased ``melt``) is the inverse: it keeps the ``id_vars`` columns
and stacks the ``value_vars`` columns into two new columns, ``variable`` (the
former column name) and ``value``. The result has ``num_rows * len(value_vars)``
rows. Stacked value columns of differing type must all be numeric and are cast
to a common type (``Float64`` if any is float, else ``Int64``).

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         DataFrame long_df = df.unpivot({"cat"}, {"p50", "p99"});
         // df.melt({"cat"}, {"p50", "p99"}) is the same op.

   .. tab-item:: Python

      .. code-block:: python

         long_df = df.unpivot(id_vars=["cat"], value_vars=["p50", "p99"])
         # df.melt(id_vars=["cat"], value_vars=["p50", "p99"]) is an alias.

   .. tab-item:: C

      .. code-block:: c

         const char* ids[]  = {"cat"};
         const char* vals[] = {"p50", "p99"};
         dftu_dataframe* long_df = dftu_dataframe_unpivot(df, ids, 1, vals, 2);

Explode a list column
---------------------

``explode`` expands a ``List`` column so each element becomes its own row, with
the other columns repeated. An empty or null list yields one null row.

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         DataFrame rows = df.explode("args");

   .. tab-item:: Python

      .. code-block:: python

         rows = df.explode("args")

   .. tab-item:: C

      .. code-block:: c

         dftu_dataframe* rows = dftu_dataframe_explode(df, "args");

``explode`` handles one ``List`` column. Its companion ``unnest``
also flattens a ``list<struct<...>>`` (each struct field becomes its own column,
named by the field) and can keep empty/null lists:

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         DataFrame rows = df.unnest("args");        // list<struct> -> one column per field
         rows = df.unnest("args", /*keep_empty=*/true);

   .. tab-item:: Python

      .. code-block:: python

         rows = df.unnest("args")                    # list<struct> -> one column per field
         rows = df.unnest("args", keep_empty=True)   # empty/null list -> one null row

   .. tab-item:: C

      .. code-block:: c

         dftu_dataframe* rows = dftu_dataframe_unnest(df, "args", /*keep_empty=*/1);

One-hot encode
--------------

``to_dummies`` replaces a column with one ``Int8`` indicator column per distinct
value (sorted ascending), named ``<column>_<value>`` and set to ``1`` where the
row held that value. The other columns pass through.

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         DataFrame onehot = df.to_dummies("cat");

   .. tab-item:: Python

      .. code-block:: python

         onehot = df.to_dummies("cat")

   .. tab-item:: C

      .. code-block:: c

         dftu_dataframe* onehot = dftu_dataframe_to_dummies(df, "cat");

See also
--------

- :doc:`time-windows` for time-bucketed rollups (``group_by_dynamic``).
- :doc:`dataframe` for project / select-rows / summarize.
- :doc:`../../cpp_api/dataframe` for the full member and C ABI reference.
