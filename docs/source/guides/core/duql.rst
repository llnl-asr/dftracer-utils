:description: Build a trace filter predicate in duql, the filter language, from Python or C++, applied as a SIMD mask and pushed down to the index at scan time.

Filter traces with duql
=======================

A duql filter is a predicate over trace events. You write it as duql text or
build it with a small typed builder, and the engine applies it as a SIMD mask
(and pushes it down to the index at scan time). The same predicate reads the
same in Python and C++, and the built predicate serializes to one canonical
duql string that the C ABI parser also accepts.

Build a predicate
-----------------

Start from a field, compare it, and combine.

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import F, TraceViewer

         # F.<name> is a field; compare and combine with & | ~
         q = (F.cat == "POSIX") & (F.dur > 1000)

         # filter() accepts the Expr directly (or a duql string):
         df = TraceViewer("traces/").filter(q).group_by("cat").agg("count").collect()

      ``F.dur`` is shorthand for ``Field("dur")``. For a nested or
      non-identifier field name, call or subscript it:
      ``F("args.level")`` or ``F["args.io.size"]``. A path descends objects by
      ``.`` and indexes arrays by ``[N]`` or a bare numeric segment
      (``args.tags[0]`` and ``args.tags.0`` are the same); a bare name (no
      ``.``/``[``) resolves top-level then under ``args``. An arg key whose name
      itself contains dots (e.g. ``cqe.raw_ns``) is a single flat member, not a
      nested object; it resolves by that flat name whether written bare
      (``F("cqe.raw_ns")``) or prefixed (``F("args.cqe.raw_ns")``), so descent
      and flat dotted keys both work. The same paths group and aggregate (see
      :doc:`../analysis/aggregation`). This is the same ``F``
      used for columnar value expressions (see :doc:`../../api/columnar`); a
      pure predicate pushes down to the index, while a predicate that mixes in
      value ops filters the scanned rows as a plan step (no index pruning).

   .. tab-item:: C++

      The one ``F`` for both filtering and value/derived columns lives in the
      dataframe layer (``dftracer::utils::dataframe::field``). ``F("name")`` is a
      field leaf; comparisons/matches build a predicate, arithmetic and the
      numeric prims build a value expression. C++ has no attribute form (no
      ``F.dur``), only ``F("dur")``.

      .. code-block:: cpp

         #include <dftracer/utils/dataframe/field.h>
         #include <dftracer/utils/trace/views/view.h>
         using namespace dftracer::utils::dataframe::field;
         using namespace dftracer::utils::trace::views;

         // A predicate pushes down to the index at scan time. View::filter()
         // takes the F expression directly (it calls .to_duql() for you):
         auto df = View::from_file("trace.pfw.gz")
                       .filter((F("cat") == "POSIX") && (F("dur") > 1000))
                       .group_by({}).agg({{AggOp::Count, "", "n"}})
                       .collect().get();
         // Or a duql string directly: View::from_file(...).duql(str)

         // The SAME F builds a value/derived column, evaluated in memory on a
         // DataFrame you already hold (resolves names -> columns, runs on the
         // SIMD engine, returns a Series):
         #include <dftracer/utils/dataframe/dataframe.h>
         Series latency_us = (F("dur") / 1000).apply(frame);
         Series mask = (F("dur") > 1000).apply(frame);   // a comparison is a mask

         // A predicate that mixes in value ops cannot push down; .to_duql()
         // (and so View::filter) throws. Evaluate it with .apply() instead:
         Series m2 = ((F("a") + F("b")) > 3).apply(frame);

      Plugins link the duql layer only and use ``duql::F``
      (``<dftracer/utils/duql/builder.h>``, ``dftracer::utils::duql``), the
      predicate-only F that renders header-only with no dataframe link. It has
      the same predicate meaning; the dataframe-layer ``F`` above is a superset
      that adds the value ops.

      .. code-block:: cpp

         // The lower-level duql-layer F (plugins, no dataframe link):
         #include <dftracer/utils/duql/builder.h>
         auto q = ((dftracer::utils::duql::F("cat") == "POSIX") &&
                   (dftracer::utils::duql::F("dur") > 1000)).build();

   .. tab-item:: C

      .. code-block:: c

         #include <dftracer/utils/duql/abi.h>

         dftu_duql* a = dftu_duql_cmp_str("cat", DFTU_DUQL_CMP_EQ, "POSIX");
         dftu_duql* b = dftu_duql_cmp_i64("dur", DFTU_DUQL_CMP_GT, 1000);
         dftu_duql* q = dftu_duql_and(a, b);   /* consumes a and b */
         /* ... dftu_dataframe_mask(q, ...) ... */
         dftu_duql_free(q);

Operators
---------

Every field supports the full set. In Python they are methods on ``Field``/``F``
(and operators for the comparisons); in C++ they are methods on ``Field`` (and
``&&`` / ``||`` / ``!``).

.. list-table::
   :header-rows: 1
   :widths: 30 35 35

   * - Predicate
     - Python
     - C++
   * - equality / inequality
     - ``F.cat == "POSIX"``, ``F.pid != 0``
     - ``Field("cat") == "POSIX"``
   * - ordering
     - ``F.dur > 1000``, ``F.ts <= end``
     - ``Field("dur") > 1000``
   * - membership
     - ``F.pid.is_in([1, 2, 3])``, ``F.cat.not_in([...])``
     - ``Field("pid").in({1, 2, 3})``
   * - SQL LIKE (``%`` any run, ``_`` one character)
     - ``F.name.like("%read%")``, ``F.name.ilike("READ")``
     - ``Field("name").like("%read%")``
   * - regex (duql dialect)
     - ``F.name.regex("^p?read$")``, ``F.name.iregex(...)``
     - ``Field("name").regex("^p?read$")``
   * - substring
     - ``F("args.file").contains("tmp")``
     - ``Field("args.file").contains("tmp")``
   * - combine
     - ``a & b``, ``a | b``, ``~a``
     - ``a && b``, ``a || b``, ``!a``

Match any element of an array
-----------------------------

``any(<path>)`` stands for any element of the array at ``<path>``: a predicate
on it holds when at least one scalar element satisfies it. It takes every
operator above.

.. tab-set::

   .. tab-item:: duql

      .. code-block:: text

         any(tags) == "gpu"
         any(sizes) > 4096
         any(tags) in ["gpu", "cpu"]
         not any(tags) == "debug"

   .. tab-item:: Python

      .. code-block:: python

         TraceViewer("logs/").filter(F.tags.any() == "gpu").collect()

   .. tab-item:: C++

      .. code-block:: cpp

         auto q = (Field("tags").any() == "gpu").build();
         // C ABI: dftu_duql_cmp_str("any(tags)", DFTU_DUQL_CMP_EQ, "gpu")

A value that is not an array, an empty array, and elements that are objects or
arrays match nothing. The index skips chunks through the evidence it keeps for
each position (``tags.0``, ``tags.1``, ...), so a membership filter prunes like
a filter on one position; an array wider than 256 positions is scanned. In
memory, ``Expr.apply`` cannot evaluate ``any()``.

Read names through row sets
---------------------------

Traces store hashes, not the full strings, for host, file path and command.
The record schema's source names row sets that map each hash to its string,
and an arrow reads a value from one: ``fhash -> files.path`` is the ``path``
of the ``files`` row whose ``fhash`` equals the event's ``fhash``. A filter on
an arrow at the top of the leading ``where`` becomes ``fhash in (keys of
files)`` on the scan, so the index still skips chunks. Arrows also work in
``select``, ``derive`` and ``group``.

.. tab-set::

   .. tab-item:: duql

      .. code-block:: text

         where fhash -> files.path like "%/scratch/%" and hhash -> hosts.name == "node01"
         | derive path = fhash -> files.path

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import TraceViewer

         tv = TraceViewer("trace.pfw.gz")
         df = tv.duql('where fhash -> files.path like "%/scratch/%"').collect()

   .. tab-item:: CLI

      .. code-block:: bash

         dftracer_view --files trace.pfw.gz \
             --duql 'where fhash -> files.path like "%/scratch/%"'

The dftracer source declares these row sets:

.. list-table::
   :header-rows: 1

   * - Row set
     - Columns
     - Arrow
   * - ``files``
     - ``fhash``, ``path``
     - ``fhash -> files.path``, ``cwd -> files(fhash).path``
   * - ``hosts``
     - ``hhash``, ``name``
     - ``hhash -> hosts.name``
   * - ``strings``
     - ``shash``, ``value``
     - ``exec_hash -> strings(shash).value``,
       ``cmd_hash -> strings(shash).value``
   * - ``ranks``
     - ``pid``, ``rank``
     - ``pid -> ranks.rank``

When the event's field is not named like the row set's key column, name the
key column in parentheses: ``files(fhash)``. ``from files`` reads a row set as
the rows of a query. The index build stores these row sets, so reading them
decodes no trace; ``explain_duql`` shows ``stored in the index``. From
Python, ``Indexer(files=["trace.pfw.gz"]).rowset("files")`` returns the
stored rows of an indexed trace as a DataFrame.

A query with no ``from`` reads the source's ``data``: for a dftracer trace,
every record but the ``ph: M`` metadata records. ``from all`` reads every
record.

Name a condition with a macro
-----------------------------

``def`` names an expression with parameters. A call expands to the body, the
arguments bound by position, before the query is planned, so a macro in a
filter pushes down like the expression it stands for.

.. code-block:: text

   def slow(t) = dur > t;
   def scratch(p) = p like "%/scratch/%";
   where slow(250us) and scratch(fhash -> files.path)

To share macros, put ``def`` lines in ``.duql`` files and list the files or
their directories in ``$DFTRACER_DUQL_PATH``, separated by ``:``. The CLI
tools also take ``--duql-path PATH`` (repeatable).

.. code-block:: bash

   echo 'def slow(t) = dur > t;' > ~/duql/io.duql
   export DFTRACER_DUQL_PATH=~/duql
   dftracer_view --files trace.pfw.gz --duql 'where slow(250us)'

A macro of the query hides one of the record schema's source, which hides one
from a path file. The rules are in :doc:`../../reference/duql`.

Shape the result with a pipeline
--------------------------------

On a trace view, a duql query can go on after the filter: stages joined by
``|`` derive, select, sort and cut the rows. Bind ``$name`` parameters
instead of formatting values into the text.

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import TraceViewer

         tv = TraceViewer("traces/")
         q = "where cat == $cat and dur > 250us | derive ms = dur / 1000 | sort -ms | take $n"
         df = tv.duql(q, cat="POSIX", n=5).collect()
         print(tv.explain_duql(q, cat="POSIX", n=5))

   .. tab-item:: C++

      .. code-block:: cpp

         using namespace dftracer::utils::trace::views;
         dftracer::utils::duql::Params p;
         p.emplace("cat", std::string("POSIX"));
         p.emplace("n", std::uint64_t{5});
         View v = View::from_file("trace.pfw.gz").duql(
             "where cat == $cat and dur > 250us"
             " | derive ms = dur / 1000 | sort -ms | take $n",
             p);

   .. tab-item:: CLI

      .. code-block:: bash

         dftracer_view --files trace.pfw.gz \
             --duql 'where cat == $cat and dur > 250us | derive ms = dur / 1000 | sort -ms | take $n' \
             --param cat='"POSIX"' --param n=5

``250us`` takes the unit of ``dur`` from the record schema. Add
``--explain`` (or call ``explain_duql``) to print the plan without scanning.
The leading ``where`` still uses the index; the stages after it run on the
scanned columns, where a missing field reads as null. The stages and their
rules are in :doc:`../../reference/duql`.

Aggregate with a pipeline
-------------------------

``group`` and ``agg`` summarize the rows, and the trace stages cut them by
time. Each recipe below is one query for ``TraceViewer.duql``,
``View::duql`` or ``dftracer_view --duql``.

Time per function
~~~~~~~~~~~~~~~~~

.. tab-set::

   .. tab-item:: duql

      .. code-block:: text

         where cat == "POSIX"
         | group name { calls = count(), total = sum(dur), p99 = quantile(dur, 0.99) }
         | sort -total
         | take 10

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import TraceViewer

         tv = TraceViewer("traces/")
         q = (
             "where cat == $cat"
             " | group name { calls = count(), total = sum(dur), p99 = quantile(dur, 0.99) }"
             " | sort -total | take 10"
         )
         df = tv.duql(q, cat="POSIX").collect()

   .. tab-item:: CLI

      .. code-block:: bash

         dftracer_view --files trace.pfw.gz --duql 'where cat == "POSIX"
             | group name { calls = count(), total = sum(dur), p99 = quantile(dur, 0.99) }
             | sort -total | take 10'

To report milliseconds, divide inside the aggregate: ``sum(dur / 1000)``.
A key can be an expression with a name: ``group name, dur > 1ms as slow
{ n = count() }``.

Busy time per 1 ms bucket
~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   where cat == "POSIX"
   | bucket 1ms
   | agg { busy = busy(), n = count() }

Each row is one bucket: ``busy`` is the time in it during which at least
one POSIX call ran, with long calls split across the buckets they cover;
``n`` counts the calls that start in it. Add ``fill`` (``bucket 1ms fill``)
to get a row for every bucket between the first and the last, empty ones
included. Put ``time_range`` first to look at one window only:

.. code-block:: text

   time_range 2s .. 3s
   | bucket 100ms
   | group name { busy = busy(), util = utilization() }

Top 3 slowest calls per file
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   where cat == "POSIX"
   | take 3 by fname sort -dur
   | select fname, name, dur

``fname`` reads ``args.fname``. Without ``sort``, ``take 3 by fname``
keeps the first three calls of each file in trace order.

A repeatable sample
~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   where cat == "POSIX" | sample 1000 seed 7 | select name, ts, dur

The same query, seed and files give the same rows, with any number of
workers. ``sample 5%`` keeps about five percent instead.

Nesting of calls
~~~~~~~~~~~~~~~~

.. code-block:: text

   where pid == 1 | call_tree | where depth == 0 | sort -dur | take 5

``call_tree`` gives each event its ``depth`` and the row number of its
``parent``, per ``pid`` and ``tid``; ``depth == 0`` keeps the outermost
calls.

Files and the slowest call of each process
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   where cat == "POSIX"
   | group pid { files = count_distinct(fname), slowest = arg_max(name, dur),
                 calls = collect(name) }

``count_distinct`` counts each file once, ``arg_max`` gives the name of the
longest call (the first one on a tie) and ``collect`` lists the calls in
trace order.

Median over stored sketches
~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   group name { s = sketch(dur) }
   | agg { p50 = quantile(merge(s), 0.5) }

``sketch`` stores each group's durations as a DDSketch; ``merge`` adds the
sketches together, so the median covers every group. Sketches that a trace
summary stored as base64 text merge the same way.

Split and slice text and arrays
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   where len(split(fname, "/")) > 3
   | derive dir = first(slice(split(fname, "/"), -2)), fields = keys(args)

``split`` gives the parts of a string, ``slice`` a part of an array and
``keys`` the field names of an object. They work in the scan filter and in
every later stage.

Check which plan a group took
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Run the query with ``--explain`` (or ``explain_duql``). A ``group`` right
after the leading ``where`` stages, with field keys and field inputs, runs
in the scan:

.. code-block:: text

   scan filter: cat == "POSIX" (pushed)
   group (trace plan): keys name; aggs calls = count(), total = sum(dur), p99 = quantile(dur, 0.99)

An expression key or input, or a stage such as ``derive`` before the
``group``, gives ``group (frame plan)``. Both plans give the same rows;
``busy``, ``concurrency``, ``utilization`` and ``active`` work only on the
trace plan. See :doc:`../../reference/duql` for the full rules.

Rank, compare and reshape
-------------------------

``window``, ``expand``, ``pivot`` and ``unpivot`` reshape the rows, and
``any`` and ``all`` test the elements of an array. The rules are in
:doc:`../../reference/duql`.

Top 3 calls per file, with ranks
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   where cat == "POSIX"
   | window fname sort -dur { r = row_number() }
   | where r <= 3
   | select fname, name, dur, r

``window`` keeps every row and adds ``r``, the position by descending
duration inside each file. Use ``rank()`` to give equal durations the same
rank.

Gap to the previous call
~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   where cat == "POSIX"
   | window pid, tid sort ts { gap = ts - lag(ts) }
   | sort -gap
   | take 10
   | select pid, tid, name, ts, gap

``lag(ts)`` is the start of the previous call of the same thread, so ``gap``
is the time since it. The first call of a thread has a null ``gap``, and
nulls sort last.

Share of a partition
~~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   where cat == "POSIX"
   | window fname { share = dur / sum(dur) }
   | select fname, name, dur, share

Without ``sort``, ``sum(dur)`` is the total of the whole partition, here one
file.

One row per array element
~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   where cat == "POSIX"
   | expand sizes as size with_index i
   | group name { n = count(), biggest = max(size) }

Each call becomes one row for each entry of ``sizes``, in order. Calls with
no entries drop out; add ``keep_empty`` to keep them with a null ``size``.

Metrics as columns
~~~~~~~~~~~~~~~~~~

.. code-block:: text

   where cat in ["POSIX", "STDIO"]
   | group name, cat { t = sum(dur) }
   | pivot cat in ["POSIX", "STDIO"] { t = sum(t) }

The result has one row for each ``name`` and the columns ``t.POSIX`` and
``t.STDIO``. A cell is null when the name has no call of that category.
Leave out ``in`` to get every value, at most ``DUQL_PIVOT_MAX_COLUMNS``
(default 1024); no stage may follow such a pivot.

Fields to rows
~~~~~~~~~~~~~~

.. code-block:: text

   where cat == "POSIX"
   | unpivot rbytes, wbytes as kind, bytes
   | group kind { total = sum(bytes) }

Each call gives one row for ``rbytes`` and one for ``wbytes``; ``kind`` holds
the field name. The listed fields must have a common type (see the
reference for the rules).

Find hosts that are down
~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   any(hosts, .state == "down")

The event matches when some element of ``hosts`` has ``state`` equal to
``"down"``. Use ``all(hosts, .state == "up")`` for the opposite test, and
``^.name`` to read a field of the event inside the test. In a leading
``where`` the test runs on each record and does not skip chunks; after the
scan it runs on list columns with the same result.

Relate records to other records
-------------------------------

A ``let`` names a row set; sub-queries, arrows (``->``), ``lookup`` and
``union`` read it by key. The rules, limits and cache are in
:doc:`../../reference/duql`.

Calls of the runs of one app
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   let runs = where type == "run";
   where type == "call" and run -> runs.app == "laghos"

The row set ``runs`` runs first. Its ``run`` values whose ``app`` is
``laghos`` become ``run in {...}`` on the scan, so chunks without them are
skipped. ``explain_duql`` shows the side and the added filter.

Files no call touched
~~~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   where type == "file"
     and path not in (from data | where type == "call" | select path)

A call without a ``path`` does not hide a file: a missing key matches
nothing.

Name each event's host
~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   let hosts = where type == "host" | select hid, host = name;
   where type == "ev"
   | lookup hosts on hid
   | group host { n = count() }

Every event stays, once; an event whose ``hid`` has no host gets a null
``host``. When one key can have several rows, read them all with
``lookup hosts on hid into hs | expand hs``.

Share of the whole trace
~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   where cat == "POSIX"
   | derive share = dur / (from data | where cat == "POSIX" | agg { t = sum(dur) })

A sub-query in an expression gives its one value.

Two traces as one
~~~~~~~~~~~~~~~~~

.. code-block:: text

   union (from "other.pfw.gz")
   | group name { n = count() }

Columns match by name. The other file is read with its own index.


How it runs
-----------

A predicate over indexed fields (``cat``, ``name``, ``pid``, ``ts``, ``dur``,
arrows into row sets) is **pushed down to the index** at scan time, so only matching
chunks are read. Anything else is evaluated as a **SIMD mask** over the columnar
batch. Either way you write the predicate once; see
:doc:`../../concepts/indexing-and-pushdown` for the pushdown model.
