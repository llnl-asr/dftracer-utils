:description: Lesson 1: create a DFTracer trace, index it, and run an aggregation query that prints a small table, in Python or C++.

Your first analysis
===================

By the end of this lesson you will have created a DFTracer trace, indexed it,
and run an aggregation query over it that prints a small table. Type every
command in order; each one builds on the last, and the expected output is shown
so you can confirm you are on track. It takes about five minutes.

Each analysis step is shown in both **Python** and **C++**: pick the tab for
your language and follow it top to bottom. The two paths read the same trace and
produce the same numbers, so a C++ reader never has to switch to Python.

This is the first of three lessons. Lesson 2, :doc:`analysis-in-depth`, builds a
real analysis on top of what you do here; lesson 3,
:doc:`extending-the-engine`, teaches you to add your own scan.

Before you start, install the package (see
:doc:`../getting-started/installation`) so that ``import dftracer.utils`` works
for the Python tabs. The C++ tabs compile against the installed headers and link
``dftracer::utils`` (same page).

Shared setup: create and index a trace
--------------------------------------

Steps 1 and 2 are language-agnostic setup: they produce one ``.pfw.gz`` trace
and its index, which both the Python and C++ analysis below read. Run them once,
in Python.

1. Create a trace
~~~~~~~~~~~~~~~~~

DFTracer traces are gzip-compressed line-delimited JSON (``.pfw.gz``). Plain
``.pfw`` is not supported; every trace you read must be gzip-compressed. Write a
tiny one so the rest of the lesson has something to read:

.. code-block:: python

   import gzip

   with gzip.open("trace.pfw.gz", "wt") as f:
       for i in range(500):
           cat = "POSIX" if i % 2 else "STDIO"
           f.write(
               f'{{"name":"read","cat":"{cat}","pid":1,"tid":1,'
               f'"ts":{1000 + i},"dur":{10 + i},"ph":"X","args":{{}}}}\n'
           )

You now have ``trace.pfw.gz`` with 500 events, alternating between the
categories ``POSIX`` and ``STDIO``.

2. Index it
~~~~~~~~~~~

Indexing builds a fast, queryable index next to the trace. It is idempotent:
running it again on an unchanged trace does nothing. The index is shared, so the
Python and C++ analysis paths both use it.

.. code-block:: python

   import dftracer.utils as dft

   with dft.Indexer(files=["trace.pfw.gz"]) as ix:
       ix.ensure_indexed()

The CLI does the same without Python: ``dftracer_index trace.pfw.gz``.

3. Group and aggregate
----------------------

Group the events by category and aggregate three ways: a row ``count``, the sum
of ``dur``, and the mean of ``dur``. The aggregate columns are named after the
spec: ``count``, ``sum_dur``, ``mean_dur``.

.. tab-set::

   .. tab-item:: Python

      ``TraceViewer`` is the entry point for reading a trace. Its builder
      methods chain; ``collect()`` builds the query plan and returns a
      ``LazyFrame``, whose own ``collect()`` runs the scan and returns the
      native ``DataFrame``.

      .. code-block:: python

         from dftracer.utils import TraceViewer

         df = (
             TraceViewer("trace.pfw.gz")
             .group_by("cat")
             .agg("count", "sum:dur", "mean:dur")
             .sort_by("cat")
             .collect()
             .collect()
         )
         print(df.to_pandas())

      Expected output (one row per category):

      .. code-block:: text

              cat  count  sum_dur  mean_dur
         0  posix    250    65000     260.0
         1  stdio    250    64750     259.0

      ``collect().collect()`` returns a native ``DataFrame``; ``to_pandas()``
      converts it at the edge, only when you ask. Note the lowercase
      ``posix``/``stdio``:
      ``group_by`` canonicalizes ``cat`` to lowercase, so a
      ``cat == "POSIX"`` filter still matches the raw events but the grouped
      output key is ``posix``.

   .. tab-item:: C++

      ``View`` is the C++ entry point (namespace
      ``dftracer::utils::trace::views``). The builder methods chain the same
      way; ``View::collect()`` builds the query plan and returns a
      ``LazyFrame``, whose own ``collect()`` runs the scan and returns a
      ``coro::CoroTask<DataFrame>`` - ``.get()`` drives that to completion for
      a non-coroutine caller like ``main()``.

      .. code-block:: cpp

         #include <dftracer/utils/trace/views/view.h>

         #include <cstdint>
         #include <cstdio>

         using namespace dftracer::utils::trace::views;

         int main() {
             auto df = View::from_file("trace.pfw.gz")
                           .group_by({GroupKey::cat()})
                           .agg({AggSpec(AggOp::Count),
                                 AggSpec(AggOp::Sum, "dur"),
                                 AggSpec(AggOp::Mean, "dur")})
                           .sort_by("cat")
                           .collect()
                           .collect()
                           .get();  // blocks; a dataframe::DataFrame

             auto cat = df.column("cat");
             auto count = df.column("count");
             auto sum_dur = df.column("sum_dur");
             auto mean_dur = df.column("mean_dur");
             for (std::int64_t i = 0; i < df.num_rows(); ++i)
                 std::printf("%-6.*s %4lld %8llu %8.1f\n",
                             static_cast<int>(cat.string_at(i).size()),
                             cat.string_at(i).data(),
                             static_cast<long long>(count.data<std::int64_t>()[i]),
                             static_cast<unsigned long long>(
                                 sum_dur.data<std::uint64_t>()[i]),
                             mean_dur.data<double>()[i]);
         }

      Expected output (one row per category):

      .. code-block:: text

         posix   250    65000    260.0
         stdio   250    64750    259.0

      The default aggregate column names are ``count``, ``sum_dur``,
      ``mean_dur``; access a column by name with ``df.column(...)`` (the
      ``DataFrame`` has no ``operator[]``). ``sum_dur`` is an unsigned 64-bit
      column (``dur`` is a non-negative field), read with
      ``data<std::uint64_t>()``. Inside async code, ``co_await`` the task
      instead of ``.get()`` - see :doc:`../concepts/coroutine-caveats`.

4. Filter before aggregating
----------------------------

Add a predicate to keep only the rows you care about. Build it with ``Field``:
the same predicate reads almost identically in Python and C++. Keep only the
``POSIX`` events, then aggregate as before.

.. tab-set::

   .. tab-item:: Python

      Compose the predicate with Python operators and pass its string form to
      ``query``:

      .. code-block:: python

         from dftracer.utils import TraceViewer, Field

         df = (
             TraceViewer("trace.pfw.gz")
             .query(str(Field("cat") == "POSIX"))
             .group_by("cat")
             .agg("count", "mean:dur")
             .collect()
             .collect()
         )
         print(df.to_pandas())

      Expected output (only the ``POSIX`` group survives the filter):

      .. code-block:: text

                 cat  count  mean_dur
            0  posix    250     260.0

   .. tab-item:: C++

      The C++ ``Field`` builder (namespace ``dftracer::utils::query``, header
      ``query/builder.h``) mirrors the Python one. ``Field("cat") == "POSIX"``
      returns an ``Expr``; ``.to_string()`` serializes it to the DSL string that
      ``View::query`` takes.

      .. code-block:: cpp

         #include <dftracer/utils/query/builder.h>
         #include <dftracer/utils/trace/views/view.h>

         #include <cstdint>
         #include <cstdio>

         using namespace dftracer::utils::trace::views;
         using dftracer::utils::query::Field;

         int main() {
             auto df = View::from_file("trace.pfw.gz")
                           .query((Field("cat") == "POSIX").to_string())
                           .group_by({GroupKey::cat()})
                           .agg({AggSpec(AggOp::Count),
                                 AggSpec(AggOp::Mean, "dur")})
                           .collect()
                           .collect()
                           .get();

             auto cat = df.column("cat");
             auto count = df.column("count");
             auto mean_dur = df.column("mean_dur");
             for (std::int64_t i = 0; i < df.num_rows(); ++i)
                 std::printf("%-6.*s %4lld %8.1f\n",
                             static_cast<int>(cat.string_at(i).size()),
                             cat.string_at(i).data(),
                             static_cast<long long>(count.data<std::int64_t>()[i]),
                             mean_dur.data<double>()[i]);
         }

      Expected output (only the ``POSIX`` group survives the filter):

      .. code-block:: text

         posix   250    260.0

5. Point at a directory
-----------------------

In practice a run produces many traces in a directory tree. Point at the
**directory** and the reader scans the tree for ``.pfw.gz`` traces in parallel.
Do not glob and pass a file list yourself; the built-in scanner is faster and
recursive. Put your trace in a folder and read the folder:

.. code-block:: python

   import os, shutil

   os.makedirs("traces", exist_ok=True)
   shutil.copy("trace.pfw.gz", "traces/trace.pfw.gz")

Now read the folder.

.. tab-set::

   .. tab-item:: Python

      Pass the directory to ``TraceViewer``:

      .. code-block:: python

         from dftracer.utils import TraceViewer

         df = (
             TraceViewer("traces")
             .group_by("cat")
             .agg("count")
             .sort_by("cat")
             .collect()
             .collect()
         )
         print(df.to_pandas())

      Expected output (both categories, counted across every ``.pfw.gz`` under
      ``traces/``):

      .. code-block:: text

                 cat  count
            0  posix    250
            1  stdio    250

   .. tab-item:: C++

      ``View::from_directory`` scans the tree in parallel. It is self-contained,
      so it returns a ``coro::CoroTask<View>``; drive it with ``.get()`` (or
      ``co_await`` it in async code) to get the ``View``, then chain as usual.

      .. code-block:: cpp

         #include <dftracer/utils/trace/views/view.h>

         #include <cstdint>
         #include <cstdio>

         using namespace dftracer::utils::trace::views;

         int main() {
             View view = View::from_directory("traces").get();
             auto df = view.group_by({GroupKey::cat()})
                           .agg({AggSpec(AggOp::Count)})
                           .sort_by("cat")
                           .collect()
                           .collect()
                           .get();

             auto cat = df.column("cat");
             auto count = df.column("count");
             for (std::int64_t i = 0; i < df.num_rows(); ++i)
                 std::printf("%-6.*s %4lld\n",
                             static_cast<int>(cat.string_at(i).size()),
                             cat.string_at(i).data(),
                             static_cast<long long>(count.data<std::int64_t>()[i]));
         }

      Expected output (both categories, counted across every ``.pfw.gz`` under
      ``traces/``):

      .. code-block:: text

         posix   250
         stdio   250

What you learned
----------------

- A trace is gzip JSON (``.pfw.gz``); **index** it once
  (``Indexer.ensure_indexed()`` or ``dftracer_index``) and both languages read
  the same index.
- **Query** it with ``TraceViewer`` (Python) or ``View`` (C++): ``group_by`` +
  ``agg`` + ``collect``, with aggregate columns named ``count`` / ``sum_dur`` /
  ``mean_dur``.
- **Filter** with a ``Field`` predicate - ``str(...)`` in Python,
  ``.to_string()`` in C++.
- Point at a **directory** to scan a whole tree in parallel.

Next, :doc:`analysis-in-depth` turns these pieces into a real analysis: derived
columns, a richer query, reshaping the result, and exporting it.
