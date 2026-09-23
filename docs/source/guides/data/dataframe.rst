:description: Get and operate on a DataFrame: build or import one, then project, filter, sort, join, and reshape it inside the native SIMD engine.

Work with DataFrames
=====================

A ``DataFrame`` is a named, ordered set of typed ``Series``: the result of a
:doc:`TraceViewer <../../trace-viewer>` query, or a frame you built or imported
yourself. Every operator on this page returns a new frame (or column) - none
mutate in place - and stays inside the native SIMD engine; Arrow, pandas, NumPy,
and polars are conversions at the edge, not the working representation. For the
op catalog in one place see :doc:`../core/columnar-ops`; for the engine
internals see :doc:`../../columnar-engine`.

Trace inputs are gzip-compressed ``.pfw.gz`` files; plain ``.pfw`` is not
supported.

Get a DataFrame
----------------

From a query, via :doc:`TraceViewer <../../trace-viewer>`:

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import TraceViewer

         df = TraceViewer("traces/").group_by("cat").agg("count", "sum:dur").collect()

   .. tab-item:: C++

      The C++ trace-query entry point is ``View``
      (``dftracer/utils/trace/views/view.h``); group keys are ``GroupKey`` and
      aggregates are ``AggSpec``. ``collect()`` is a coroutine - ``.get()``
      drives it to completion for a non-coroutine caller.

      .. code-block:: cpp

         #include <dftracer/utils/trace/views/view.h>
         using namespace dftracer::utils::trace::views;

         auto df = View::from_file("trace.pfw.gz")
                       .group_by({GroupKey::cat()})
                       .agg({{AggOp::Count, "", "count"},
                             {AggOp::Sum, "dur", "sum_dur"}})
                       .collect()   // -> LazyFrame
                       .collect()   // -> coro::CoroTask<DataFrame>
                       .get();

      See :doc:`../analysis/aggregation` for the full group-key and aggregate
      vocabulary.

From data you already have, in Python:

.. code-block:: python

   from dftracer.utils import DataFrame

   df = DataFrame.from_pandas(pandas_df)
   df = DataFrame.from_arrow(pyarrow_table)
   df = DataFrame.from_polars(polars_df)
   df = DataFrame.from_parquet("data.parquet", columns=["dur", "cat"])
   df = DataFrame.from_dict({"cat": ["POSIX", "STDIO"], "dur": [12, 34]})
   df = DataFrame.from_numpy(arr, columns=["a", "b"])  # 2-D array or {name: 1-D array}

In C++, build a frame from ``Series`` you already hold:

.. code-block:: cpp

   #include <dftracer/utils/dataframe/dataframe.h>

   using namespace dftracer::utils::dataframe;

   DataFrame df{{"cat", "dur"}, {std::move(cat_series), std::move(dur_series)}};

There is also a flat C ABI (``dftu_dataframe_*`` in
``dftracer/utils/dataframe/abi.h``) for building and operating on frames from a
foreign language or a plugin; see :doc:`../../cpp_api/dataframe`.

Project columns
----------------

Pick, rename, or add columns without touching rows.

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         narrow = df.select("cat", "count")
         renamed = df.rename({"count": "n"})
         extended = df.with_column("avg_dur", (df["sum_dur"] / df["count"]))

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/dataframe/field.h>
         using namespace dftracer::utils::dataframe::field;

         DataFrame narrow = df.select({"cat", "count"});
         DataFrame renamed = df.rename({"cat", "n"});  // positional: one new name per column

         // A derived column, name-based, via the unified F (resolves names ->
         // columns and runs on the SIMD engine):
         Series avg_dur = (F("sum_dur") / F("count")).apply(df);
         DataFrame extended = df.with_column("avg_dur", avg_dur);
         // Or eager per-column arithmetic when you already hold the Series:
         Series avg2 = df.column("sum_dur") / df.column("count");

``rename`` in C++ takes the full positional list of new names (one per existing
column, in order); the Python wrapper takes a ``{old: new}`` mapping.

For evaluating several derived columns in one compiled pass (shared
subexpressions computed once), the lower-level index-based ``col(i)`` API with
``eval_many`` (``<dftracer/utils/dataframe/expr.h>``) is available; the
name-based ``F`` covers the common single-column case.

Select rows
------------

Filter, take the top/bottom rows, sort, sample, or slice.

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         posix = df.filter(df["cat"].str_eq("POSIX"))
         worst = df.sort_by("sum_dur", descending=True).head(10)
         top10 = df.topk("sum_dur", 10)             # equivalent to sort_by + head, one pass
         first_page = df.slice(0, 100)
         some = df.sample(50, seed=0)

   .. tab-item:: C++

      .. code-block:: cpp

         DataFrame posix = df.filter(df.column("cat").str_eq("POSIX"));
         DataFrame worst = df.sort_by("sum_dur", /*descending=*/true).head(10);
         DataFrame top10 = df.topk("sum_dur", 10);
         DataFrame first_page = df.slice(0, 100);
         DataFrame some = df.sample(50, /*seed=*/0);

``filter`` takes a boolean mask ``Series`` the same length as the frame -
usually the output of a comparison or string predicate on one of its columns.
Multi-key sort is ``sort_by_multi(names, descending=false)``.

Reshape
--------

Drop or fill nulls, deduplicate, or change the frame's shape.

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         clean = df.drop_nulls()
         filled = df.fill_null(0)
         distinct = df.unique()                      # drop_duplicates() is an alias
         dup_mask = df.is_duplicated()                # also is_unique(); a Series
         long = df.unpivot(id_vars=["cat"], value_vars=["p50", "p99"])  # melt() is an alias
         rows = df.explode("args")                    # a List column, one row per element
         onehot = df.to_dummies("cat")
         wide = df.pivot(index="ts", columns="cat", values="dur", agg="sum")

   .. tab-item:: C++

      .. code-block:: cpp

         DataFrame clean = df.drop_nulls();
         DataFrame filled = df.fill_null(0);  // natural values, no dftu_scalar
         DataFrame distinct = df.unique();
         Series dup_mask = df.is_duplicated();  // also is_unique()
         DataFrame long_df = df.unpivot({"cat"}, {"p50", "p99"});
         DataFrame rows = df.explode("args");
         DataFrame onehot = df.to_dummies("cat");
         DataFrame wide = df.pivot("ts", "cat", "dur", Agg::Sum);

``pivot``'s ``agg`` collision reducer is one of ``first`` / ``last`` / ``sum`` /
``min`` / ``max`` / ``mean`` (``mean`` yields ``Float64``); it defaults to
``"first"``. In C++ it takes either the string or an ``Agg`` enumerator
(``Agg::First``, ``Agg::Sum``, ...). ``is_duplicated`` / ``is_unique`` hash the
whole row (like ``unique``) and return a boolean mask the length of the frame.

Summarize
----------

Aggregate by group, over time windows, or describe every column at once.

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         by_cat = df.group_by("cat", "count", "sum:dur")
         windows = df.group_by_dynamic("ts", every=1_000_000, period=0,
                                        aggs=["count", "mean:dur"])
         stats = df.describe()
         nulls = df.null_count()

   .. tab-item:: C++

      .. code-block:: cpp

         DataFrame by_cat = df.group_by("cat", {{Agg::Sum, "dur", "sum_dur"},
                                                {Agg::Count, "", "count"}});
         DataFrame windows = df.group_by_dynamic("ts", /*every=*/1'000'000,
                                                  /*period=*/0, aggs);
         DataFrame stats = df.describe();
         DataFrame nulls = df.null_count();

``group_by_dynamic`` windows are tumbling/sliding over an ascending Int64 time
column: they start at the first timestamp floored to a multiple of ``every``,
stride by ``every``, and each covers ``[start, start + period)``; ``period <=
0`` means "same as ``every``" (tumbling). ``GroupAgg`` in C++ is ``{op, column,
out}`` where ``op`` is an ``Agg`` enumerator: ``Agg::Count`` (column ignored),
``Agg::Sum``, ``Agg::Min``, ``Agg::Max``, ``Agg::Mean``, and the higher moments
``Agg::Var`` / ``Agg::Std`` / ``Agg::Skew`` / ``Agg::Kurt``.

Relational
-----------

Hash-join two frames on one or more key columns (by name). See :doc:`joins`
for the join kinds, the output column naming, the lazy form and joining
aggregated queries.

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         using dftracer::utils::dataframe::JoinHow;

         DataFrame joined = df.join(other, {"fid"}, JoinHow::Inner);

   .. tab-item:: Python

      .. code-block:: python

         # how is inner/left/right/outer/semi/anti/cross.
         joined = df.join(other, on="fid", how="inner")

To hash-partition a frame's rows into ``n_parts`` buckets by one or more key
columns (the shuffle primitive behind a distributed ``group_by``/join: send
each part to the worker that owns it, then join/group locally):

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         parts = df.hash_partition(["pid"], n_parts=4)  # list[DataFrame]

   .. tab-item:: C++

      ``hash_partition`` lives in ``src/dftracer/utils/dataframe/batch_ops.h``,
      an in-tree header (not installed under ``include/``); it is reachable from
      code built inside this repository, not from an external consumer linking
      only against the installed public headers.

      .. code-block:: cpp

         #include <dftracer/utils/dataframe/batch_ops.h>

         std::vector<DataFrame> parts = hash_partition(df, {"pid"}, 4);

``keys`` may be one or more column names; the same key always lands in the
same part (stable across calls), so parts from two frames partitioned on the
same keys and part count can be joined/grouped locally.

Convert out (only at the edge)
--------------------------------

.. code-block:: python

   df.to_arrow()             # pyarrow.Table (zero-copy)
   df.to_pandas()            # pandas DataFrame (NumPy dtypes: a copy)
   df.to_pandas(arrow=True)  # pandas DataFrame over Arrow buffers (no copy)
   df.to_polars()            # polars DataFrame (numbers shared, strings copied)

Every conversion is at the edge: everything above this line stays inside the
SIMD engine, and the round trip through Arrow only happens when you ask for it.

See also
---------

- :doc:`lazyframe` for the deferred form: plans over any source, pushdown,
  streaming under a memory budget, a plugin's own step.
- :doc:`series` for the column-level how-to (arithmetic, reducers, strings,
  derived columns via ``F``).
- :doc:`pandas-polars` for the pandas and polars spellings (the index model,
  ``loc`` / ``iloc``, group-by functions, the accessors, expression column
  ops) and what is left out on purpose.
- :doc:`joins` for joining frames, :doc:`reshape` for pivot / unpivot / explode /
  one-hot, and :doc:`time-windows` for time-bucketed rollups.
- :doc:`../analysis/aggregation` for aggregating a trace query into a frame.
- :doc:`../core/columnar-ops` for the full op catalog in one place.
- :doc:`../../cpp_api/dataframe` for the generated C++ member reference and the
  C ABI.
- :doc:`../../columnar-engine` for how the engine is built (encodings, SIMD
  kernels, the Arrow bridge).
