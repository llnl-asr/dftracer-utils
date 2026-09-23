:description: A C++-first lesson: query, aggregate, and read a trace end to end against the View engine, from producing a trace to printing the result.

Analyze a trace in C++
======================

The other tutorials lead with Python and show C++ as a second tab. This lesson
is the opposite: everything from the query onward is C++, start to finish,
against the ``View`` engine (namespace ``dftracer::utils::trace::views``,
header ``dftracer/utils/trace/views/view.h``). It assumes
:doc:`first-analysis`, so the concepts (index, group-by, aggregate) are not
re-explained here - only the C++ surface.

Compile every snippet below against the installed headers, linking
``dftracer::utils`` (``find_package(dftracer_utils REQUIRED)`` +
``target_link_libraries(my_tool PRIVATE dftracer::utils)``); see
:doc:`../getting-started/installation` for the CMake setup.

1. Produce a trace
-------------------

One piece of setup is not C++: writing the gzip-JSON trace file itself. DFTracer
traces are gzip-compressed line-delimited JSON (``.pfw.gz``); the fastest way to
get one on disk for this lesson is a few lines of Python, exactly as in
:doc:`first-analysis`:

.. code-block:: python

   import gzip

   with gzip.open("trace.pfw.gz", "wt") as f:
       for i in range(500):
           cat = "POSIX" if i % 2 else "STDIO"
           f.write(
               f'{{"name":"read","cat":"{cat}","pid":1,"tid":1,'
               f'"ts":{1000 + i},"dur":{10 + i},"ph":"X","args":{{}}}}\n'
           )

Everything from here on is C++.

2. Load the trace and filter with the fluent builder
------------------------------------------------------

``View::from_file`` opens one trace (building its index on first touch).
Build a predicate with the unified ``F`` (namespace
``dftracer::utils::dataframe::field``, header
``dftracer/utils/dataframe/field.h``): ``F("dur") < 300`` is a field predicate
that ``View::filter`` takes directly and pushes down to the index. The same
``F`` also builds value/derived columns (see step 5). ``View::collect()``
builds the query plan and returns a ``LazyFrame``; its own ``collect()`` runs
the scan and returns the ``coro::CoroTask<DataFrame>`` that ``.get()`` drives
to completion, hence the ``.collect().collect().get()`` chain below.

.. code-block:: cpp

   #include <dftracer/utils/dataframe/field.h>
   #include <dftracer/utils/trace/views/view.h>

   #include <cstdint>
   #include <cstdio>

   using namespace dftracer::utils::trace::views;
   using dftracer::utils::dataframe::field::F;

   int main() {
       auto df = View::from_file("trace.pfw.gz")
                     .filter(F("dur") < 300)
                     .group_by({GroupKey::cat()})
                     .agg({AggSpec(AggOp::Count), AggSpec(AggOp::Sum, "dur")})
                     .sort_by("cat")
                     .collect()
                     .collect()
                     .get();  // blocks; a dataframe::DataFrame

       auto cat = df.column("cat");
       auto count = df.column("count");
       auto sum_dur = df.column("sum_dur");
       for (std::int64_t i = 0; i < df.num_rows(); ++i)
           std::printf("%-6.*s %4lld %8llu\n",
                       static_cast<int>(cat.string_at(i).size()),
                       cat.string_at(i).data(),
                       static_cast<long long>(count.data<std::int64_t>()[i]),
                       static_cast<unsigned long long>(
                           sum_dur.data<std::uint64_t>()[i]));
   }

Expected output (only the 290 events with ``dur < 300`` survive the filter,
split by category):

.. code-block:: text

   posix   145    22475
   stdio   145    22330

A ``cat`` group key is always folded to lowercase (so ``POSIX`` and ``posix``
land in the same group even if a producer is inconsistent); the input JSON's
casing does not survive into the result. ``df.column(name)`` returns a
``Series``; there is no ``operator[]`` on ``DataFrame``. ``sum_dur`` here is an
unsigned 64-bit column (``dur`` is a non-negative field); read it with
``data<std::uint64_t>()``, not ``std::int64_t``. Inside async code,
``co_await`` the task in place of ``.get()`` - see
:doc:`../concepts/coroutine-caveats`.

3. Reduce a column to a scalar
-------------------------------

A ``Series`` has its own reducers - ``sum()``, ``min()``, ``max()`` - each
returning a ``Scalar`` (header ``dftracer/utils/dataframe/scalar.h``). Read it
back typed with ``.f64()`` or ``.i64()``:

.. code-block:: cpp

   #include <dftracer/utils/trace/views/view.h>

   #include <cstdio>

   using namespace dftracer::utils::trace::views;

   int main() {
       auto df = View::from_file("trace.pfw.gz")
                     .group_by({GroupKey::cat()})
                     .agg({AggSpec(AggOp::Sum, "dur")})
                     .collect()
                     .collect()
                     .get();

       auto sum_dur = df.column("sum_dur");
       double grand_total = sum_dur.sum().f64();
       std::printf("total dur across both categories: %.1f\n", grand_total);
   }

Expected output (the full, unfiltered trace: POSIX 65000 + STDIO 64750):

.. code-block:: text

   total dur across both categories: 129750.0

4. A custom fold with ViewSession
------------------------------------

The built-in ``AggSpec`` ops cover common reductions, but sometimes you want a
one-off accumulation without writing a full plugin (see
:doc:`write-a-c-plugin` for that path). ``View::session()`` opens a
``ViewSession``: register one or more ops, then ``execute()`` runs a single
shared scan and resolves every registered ``Deferred`` handle at once.
``ViewSession::fold<P>`` folds each matching event's raw JSON into a partial
``P``, combining per-worker partials with the function you supply:

.. code-block:: cpp

   #include <dftracer/utils/dataframe/field.h>
   #include <dftracer/utils/json/json_value.h>
   #include <dftracer/utils/trace/views/view.h>

   #include <cstdio>

   using namespace dftracer::utils::trace::views;
   using dftracer::utils::dataframe::field::F;
   using dftracer::utils::json::JsonValue;

   int main() {
       View view = View::from_file("trace.pfw.gz");
       ViewSession run = view.session();

       Deferred<double> posix_dur_sum = run.fold<double>(
           F("cat") == "POSIX",
           [](double& acc, const JsonValue& jv, std::string_view /*raw*/) {
               acc += jv["dur"].get<double>(0.0);
           },
           [](double&& a, double&& b) { return a + b; });

       run.execute().get();  // one shared scan; resolves posix_dur_sum
       std::printf("POSIX dur sum: %.1f\n", *posix_dur_sum);
   }

Expected output (matches the ``sum_dur`` a ``group_by("cat")`` would give the
``posix`` group over the full trace):

.. code-block:: text

   POSIX dur sum: 65000.0

The predicate here matches the raw JSON field, not a group key, so its case
sensitivity is unaffected by the lowercasing ``group_by({GroupKey::cat()})``
does in step 2: ``F("cat") == "POSIX"`` only matches events whose ``cat`` field
is literally ``"POSIX"``. ``fold`` accepts the ``F`` predicate directly (it
derives the pushdown ``Query`` via ``.to_query()``); a predicate that mixes in
value ops is not pushable and throws. ``Deferred<P>::get()`` (or ``*``/``->``)
throws if read before ``execute()``
has resolved it - that is what makes ``run.execute().get()`` a hard
prerequisite, not an optimization.

What you learned
-----------------

- ``View::from_file`` / ``View::from_directory`` open a trace; the unified
  ``F("field")`` (``dftracer/utils/dataframe/field.h``) builds a predicate that
  ``View::filter`` / ``fold`` take directly (pushdown), and the same ``F`` builds
  value/derived columns via ``.apply(df)``.
- ``group_by`` + ``agg`` + ``collect().collect().get()`` gives a ``DataFrame``; read a
  column with ``df.column(name)`` and reduce it with ``Series::sum()`` /
  ``min()`` / ``max()``, each returning a ``Scalar`` read back with ``.f64()``
  / ``.i64()``.
- For a one-off computation the built-in ``AggOp`` set does not cover, open a
  ``ViewSession`` with ``View::session()`` and register a ``fold<P>`` op;
  ``execute()`` runs the one shared scan and resolves every registered
  ``Deferred``.

For the built-in aggregates in depth, see :doc:`../guides/analysis/views`; for
a computation that needs its own mergeable state and runs as a first-class
plugin instead of an inline fold, see :doc:`write-a-c-plugin`.
