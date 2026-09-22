:description: Lesson 2: turn one aggregation into a complete analysis with a richer query, a derived column, reshaping, a summary number, and an export.

Analysis in depth
=================

In :doc:`first-analysis` you ran a single aggregation. This lesson turns that
into a small but complete analysis: a richer query, a derived column you compute
yourself, a couple of reshaping steps, a summary number, and an export. Type
each block in order; this lesson assumes you have finished lesson 1.

Every analysis step is shown in both **Python** and **C++**: pick your tab and
follow it. The two paths read the same trace and produce the same numbers. Every
step runs on one trace you build in step 1, and the expected output is shown
after each command.

1. Build a richer trace
-----------------------

This is shared, language-agnostic setup. The lesson-1 trace had a single event
name; here we want several, so grouping and filtering have something to
separate. Write three event kinds, each with its own duration:

.. code-block:: python

   import gzip

   DURS = {"read": 100, "write": 200, "open": 50}
   NAMES = ["read", "write", "open"]

   with gzip.open("app.pfw.gz", "wt") as f:
       for i in range(300):
           name = NAMES[i % 3]
           f.write(
               f'{{"name":"{name}","cat":"POSIX","pid":1,"tid":1,'
               f'"ts":{1000 + i},"dur":{DURS[name]},"ph":"X","args":{{}}}}\n'
           )

This is 300 events: 100 each of ``read`` (dur 100), ``write`` (dur 200), and
``open`` (dur 50). The reader indexes it on first touch, so there is no separate
index step.

2. Aggregate everything
-----------------------

Start from the full picture. Group by event name, aggregate, and sort the result
by name so the output is stable.

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import TraceViewer

         df = (
             TraceViewer("app.pfw.gz")
             .group_by("name")
             .agg("count", "sum:dur", "mean:dur")
             .sort_by("name")
             .collect()
             .collect()
         )
         print(df.to_pandas())

      Expected output:

      .. code-block:: text

               name  count  sum_dur  mean_dur
          0    open    100     5000      50.0
          1    read    100    10000     100.0
          2   write    100    20000     200.0

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/trace/views/view.h>

         #include <cstdint>
         #include <cstdio>

         using namespace dftracer::utils::trace::views;

         int main() {
             auto df = View::from_file("app.pfw.gz")
                           .group_by({GroupKey::name()})
                           .agg({AggSpec(AggOp::Count),
                                 AggSpec(AggOp::Sum, "dur"),
                                 AggSpec(AggOp::Mean, "dur")})
                           .sort_by("name")
                           .collect()
                           .collect()
                           .get();

             auto name = df.column("name");
             auto count = df.column("count");
             auto sum_dur = df.column("sum_dur");
             auto mean_dur = df.column("mean_dur");
             for (std::int64_t i = 0; i < df.num_rows(); ++i)
                 std::printf("%-6.*s %4lld %8llu %8.1f\n",
                             static_cast<int>(name.string_at(i).size()),
                             name.string_at(i).data(),
                             static_cast<long long>(count.data<std::int64_t>()[i]),
                             static_cast<unsigned long long>(
                                 sum_dur.data<std::uint64_t>()[i]),
                             mean_dur.data<double>()[i]);
         }

      Expected output:

      .. code-block:: text

         open    100     5000     50.0
         read    100    10000    100.0
         write   100    20000    200.0

3. Query with a predicate
-------------------------

Now narrow to the events that matter. Predicates compose with and/or/not and
offer string matchers such as ``like`` (SQL wildcards) and ``contains``
(substring). Keep the events whose name contains the letter ``r`` and whose
duration is at least 100 microseconds.

.. tab-set::

   .. tab-item:: Python

      The Python DSL composes with ``&`` (and), ``|`` (or), and ``~`` (not):

      .. code-block:: python

         from dftracer.utils import TraceViewer, Field

         q = Field("name").like("%r%") & (Field("dur") >= 100)

         df = (
             TraceViewer("app.pfw.gz")
             .query(str(q))
             .group_by("name")
             .agg("count", "sum:dur", "mean:dur")
             .sort_by("name")
             .collect()
             .collect()
         )
         print(df.to_pandas())

      Expected output (``open`` is dropped: it has no ``r`` and its duration is
      below the threshold):

      .. code-block:: text

               name  count  sum_dur  mean_dur
          0    read    100    10000     100.0
          1   write    100    20000     200.0

      The DSL also has virtual **resolved** fields for path-like metadata the
      index stores by hash - for example ``resolved("fpath").like("%/scratch/%")``
      or ``resolved("hostname") == "node01"``. Those need traces that carry the
      matching metadata, so they are not part of this lesson's path; see the
      :doc:`../guides/core/query-dsl` guide for the full operator list.

   .. tab-item:: C++

      The C++ ``Field`` builder composes with ``&&``, ``||``, and ``!`` (the
      C++ operators) and has the same matchers. ``.to_string()`` hands the built
      expression to ``View::query``.

      .. code-block:: cpp

         #include <dftracer/utils/query/builder.h>
         #include <dftracer/utils/trace/views/view.h>

         #include <cstdint>
         #include <cstdio>

         using namespace dftracer::utils::trace::views;
         using dftracer::utils::query::Field;

         int main() {
             auto q = Field("name").like("%r%") && (Field("dur") >= 100);

             auto df = View::from_file("app.pfw.gz")
                           .query(q.to_string())
                           .group_by({GroupKey::name()})
                           .agg({AggSpec(AggOp::Count),
                                 AggSpec(AggOp::Sum, "dur"),
                                 AggSpec(AggOp::Mean, "dur")})
                           .sort_by("name")
                           .collect()
                           .collect()
                           .get();

             auto name = df.column("name");
             auto count = df.column("count");
             auto sum_dur = df.column("sum_dur");
             auto mean_dur = df.column("mean_dur");
             for (std::int64_t i = 0; i < df.num_rows(); ++i)
                 std::printf("%-6.*s %4lld %8llu %8.1f\n",
                             static_cast<int>(name.string_at(i).size()),
                             name.string_at(i).data(),
                             static_cast<long long>(count.data<std::int64_t>()[i]),
                             static_cast<unsigned long long>(
                                 sum_dur.data<std::uint64_t>()[i]),
                             mean_dur.data<double>()[i]);
         }

      Expected output (``open`` is dropped: it has no ``r`` and its duration is
      below the threshold):

      .. code-block:: text

         read    100    10000    100.0
         write   100    20000    200.0

      ``resolved("name")`` builds the same resolved-hash fields as the Python
      helper; see :doc:`../guides/core/query-dsl`.

4. Derive a column yourself
---------------------------

The aggregation already gave you ``mean_dur``, but you will often need a metric
the aggregator did not compute. Build one from the columns you have and attach
it to the collected frame. Recreate the mean from ``sum_dur`` and ``count`` to
see it match.

.. tab-set::

   .. tab-item:: Python

      Use ``F`` to name columns, combine them arithmetically, and evaluate the
      expression against the frame with ``df.apply(...)`` to get a ``Series``
      back:

      .. code-block:: python

         from dftracer.utils import TraceViewer, F

         df = (
             TraceViewer("app.pfw.gz")
             .group_by("name")
             .agg("count", "sum:dur")
             .sort_by("name")
             .collect()
             .collect()
         )

         avg = df.apply(F.sum_dur / F.count)     # a Series
         df = df.with_column("avg_dur", avg)     # attach it as a new column
         print(df.to_pandas())

      Expected output:

      .. code-block:: text

               name  count  sum_dur  avg_dur
          0    open    100     5000     50.0
          1    read    100    10000    100.0
          2   write    100    20000    200.0

   .. tab-item:: C++

      C++ has no column-expression DSL; compute the derived column directly with
      ``Series`` arithmetic and attach it with ``with_column``. Cast to
      ``Float64`` first so the division is a true (non-integer) mean.

      .. code-block:: cpp

         #include <dftracer/utils/dataframe/dataframe.h>
         #include <dftracer/utils/trace/views/view.h>

         #include <cstdint>
         #include <cstdio>

         using namespace dftracer::utils::trace::views;
         using dftracer::utils::dataframe::Series;
         using dftracer::utils::dataframe::TypeId;

         int main() {
             auto df = View::from_file("app.pfw.gz")
                           .group_by({GroupKey::name()})
                           .agg({AggSpec(AggOp::Count),
                                 AggSpec(AggOp::Sum, "dur")})
                           .sort_by("name")
                           .collect()
                           .collect()
                           .get();

             Series avg = df.column("sum_dur").cast(TypeId::Float64)
                              .div(df.column("count").cast(TypeId::Float64));
             df = df.with_column("avg_dur", avg);

             auto name = df.column("name");
             auto count = df.column("count");
             auto sum_dur = df.column("sum_dur");
             auto avg_dur = df.column("avg_dur");
             for (std::int64_t i = 0; i < df.num_rows(); ++i)
                 std::printf("%-6.*s %4lld %8llu %8.1f\n",
                             static_cast<int>(name.string_at(i).size()),
                             name.string_at(i).data(),
                             static_cast<long long>(count.data<std::int64_t>()[i]),
                             static_cast<unsigned long long>(
                                 sum_dur.data<std::uint64_t>()[i]),
                             avg_dur.data<double>()[i]);
         }

      Expected output:

      .. code-block:: text

         open    100     5000     50.0
         read    100    10000    100.0
         write   100    20000    200.0

5. Add several derived columns
------------------------------

You will often want more than one derived column. Add both an average and the
total duration in milliseconds.

.. tab-set::

   .. tab-item:: Python

      When you have several expressions, ``eval_many`` compiles them into a
      single pass and shares any common subexpression, instead of walking the
      columns once per expression. It returns one ``Series`` per input
      expression, in order:

      .. code-block:: python

         from dftracer.utils import eval_many, F

         avg_dur, total_ms = eval_many(
             [F.sum_dur / F.count, F.sum_dur / 1000],
             df,
         )
         df = df.with_column("avg_dur", avg_dur).with_column("total_ms", total_ms)
         print(df.to_pandas())

      Expected output (``total_ms`` is ``sum_dur`` in milliseconds):

      .. code-block:: text

               name  count  sum_dur  avg_dur  total_ms
          0    open    100     5000     50.0       5.0
          1    read    100    10000    100.0      10.0
          2   write    100    20000    200.0      20.0

   .. tab-item:: C++

      Build each column with ``Series`` arithmetic and attach them in turn.
      Scalar division (``/ 1000.0``) broadcasts the constant across the column.

      .. code-block:: cpp

         #include <dftracer/utils/dataframe/dataframe.h>
         #include <dftracer/utils/trace/views/view.h>

         #include <cstdint>
         #include <cstdio>

         using namespace dftracer::utils::trace::views;
         using dftracer::utils::dataframe::DataFrame;
         using dftracer::utils::dataframe::Series;
         using dftracer::utils::dataframe::TypeId;

         int main() {
             auto df = View::from_file("app.pfw.gz")
                           .group_by({GroupKey::name()})
                           .agg({AggSpec(AggOp::Count),
                                 AggSpec(AggOp::Sum, "dur")})
                           .sort_by("name")
                           .collect()
                           .collect()
                           .get();

             Series sum_f = df.column("sum_dur").cast(TypeId::Float64);
             Series avg_dur = sum_f.div(df.column("count").cast(TypeId::Float64));
             Series total_ms = df.column("sum_dur").cast(TypeId::Float64) / 1000.0;
             df = df.with_column("avg_dur", avg_dur)
                      .with_column("total_ms", total_ms);

             auto name = df.column("name");
             auto count = df.column("count");
             auto sum_dur = df.column("sum_dur");
             auto avg = df.column("avg_dur");
             auto tms = df.column("total_ms");
             for (std::int64_t i = 0; i < df.num_rows(); ++i)
                 std::printf("%-6.*s %4lld %8llu %8.1f %8.1f\n",
                             static_cast<int>(name.string_at(i).size()),
                             name.string_at(i).data(),
                             static_cast<long long>(count.data<std::int64_t>()[i]),
                             static_cast<unsigned long long>(
                                 sum_dur.data<std::uint64_t>()[i]),
                             avg.data<double>()[i], tms.data<double>()[i]);
         }

      Expected output (``total_ms`` is ``sum_dur`` in milliseconds):

      .. code-block:: text

         open    100     5000     50.0      5.0
         read    100    10000    100.0     10.0
         write   100    20000    200.0     20.0

6. Reshape and summarize
------------------------

The result is an ordinary frame with the usual operations. Rank the event kinds
by total duration, largest first, then reduce a single column to one number.

.. tab-set::

   .. tab-item:: Python

      ``sort_by`` orders the rows; a ``Series`` reduces to a scalar. Pull one
      column out with ``df[...]`` and reduce it:

      .. code-block:: python

         ranked = df.sort_by("sum_dur", descending=True)
         print(ranked.to_pandas())
         print("events:", df["count"].sum())
         print("slowest mean:", df["avg_dur"].max())

      Expected output:

      .. code-block:: text

               name  count  sum_dur  avg_dur  total_ms
          0   write    100    20000    200.0      20.0
          1    read    100    10000    100.0      10.0
          2    open    100     5000     50.0       5.0
          events: 300
          slowest mean: 200.0

   .. tab-item:: C++

      ``sort_by`` takes a ``descending`` flag; the ``Series`` reducers return a
      typed ``Scalar`` - read it with ``.i64()`` / ``.f64()`` / ``.as<T>()``,
      never a raw union member.

      .. code-block:: cpp

         // Continues from step 5, with avg_dur and total_ms attached to df.
         DataFrame ranked = df.sort_by("sum_dur", /*descending=*/true);

         auto name = ranked.column("name");
         auto sum_dur = ranked.column("sum_dur");
         for (std::int64_t i = 0; i < ranked.num_rows(); ++i)
             std::printf("%-6.*s %8llu\n",
                         static_cast<int>(name.string_at(i).size()),
                         name.string_at(i).data(),
                         static_cast<unsigned long long>(
                             sum_dur.data<std::uint64_t>()[i]));

         std::printf("events: %lld\n",
                     static_cast<long long>(df.column("count").sum().i64()));
         std::printf("slowest mean: %.1f\n",
                     df.column("avg_dur").max().f64());

      Expected output:

      .. code-block:: text

         write     20000
         read      10000
         open       5000
         events: 300
         slowest mean: 200.0

7. Export at the edge
---------------------

Keep the data in the native engine while you work, then convert only when you
hand it to another library. This is a Python-edge concern: a ``DataFrame``
converts to pandas, Arrow, or polars there.

.. code-block:: python

   pdf = df.to_pandas()          # a pandas.DataFrame
   table = df.to_arrow()         # a pyarrow.Table
   print(type(pdf).__name__, type(table).__name__)

Expected output:

.. code-block:: text

   DataFrame Table

In C++ there is no edge to cross: ``collect()`` already returns the native
columnar ``dataframe::DataFrame``, and you read a column's buffer directly with
``df.column(name).data<T>()`` (as the tabs above do), so no conversion is
needed.

What you learned
----------------

- Compose query predicates with ``&`` / ``|`` / ``~`` in Python (``&&`` / ``||``
  / ``!`` in C++) and matchers like ``like`` and ``contains``.
- Build a derived column: ``df.apply(F...)`` in Python, ``Series`` arithmetic +
  ``with_column`` in C++.
- Reshape with ``sort_by`` and reduce a ``Series`` to a scalar.
- Convert to pandas / Arrow at the edge in Python; C++ already holds the native
  frame.

For the full frame and column reference, see :doc:`../guides/data/dataframe` and
:doc:`../guides/core/columnar-ops`. Next, :doc:`extending-the-engine` shows how
to add your own scan when the built-in aggregates are not enough.
