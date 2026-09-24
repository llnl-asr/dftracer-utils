:description: Build a trace filter predicate with the typed DSL in Python or C++, applied as a SIMD mask and pushed down to the index at scan time.

Filter traces with the query DSL
================================

A query is a predicate over trace events. You build it with a small typed DSL,
and the engine applies it as a SIMD mask (and pushes it down to the index at
scan time). The same predicate reads the same in Python and C++, and the built
query serializes to one canonical string that the C ABI parser also accepts.

Build a predicate
-----------------

Start from a field, compare it, and combine.

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import F, TraceViewer

         # F.<name> is a field; compare and combine with & | ~
         q = (F.cat == "POSIX") & (F.dur > 1000)

         # filter()/query() accept the Expr directly (or a DSL string):
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
         // takes the F expression directly (it calls .to_query() for you):
         auto df = View::from_file("trace.pfw.gz")
                       .filter((F("cat") == "POSIX") && (F("dur") > 1000))
                       .group_by({}).agg({{AggOp::Count, "", "n"}})
                       .collect().get();
         // Or a DSL string directly: View::from_file(...).query(str)

         // The SAME F builds a value/derived column, evaluated in memory on a
         // DataFrame you already hold (resolves names -> columns, runs on the
         // SIMD engine, returns a Series):
         #include <dftracer/utils/dataframe/dataframe.h>
         Series latency_us = (F("dur") / 1000).apply(frame);
         Series mask = (F("dur") > 1000).apply(frame);   // a comparison is a mask

         // A predicate that mixes in value ops cannot push down; .to_query()
         // (and so View::filter) throws. Evaluate it with .apply() instead:
         Series m2 = ((F("a") + F("b")) > 3).apply(frame);

      Plugins link the query layer only and use ``query::F``
      (``<dftracer/utils/query/builder.h>``, ``dftracer::utils::query``), the
      predicate-only F that renders header-only with no dataframe link. It has
      the same predicate meaning; the dataframe-layer ``F`` above is a superset
      that adds the value ops.

      .. code-block:: cpp

         // The lower-level query-layer F (plugins, no dataframe link):
         #include <dftracer/utils/query/builder.h>
         auto q = ((dftracer::utils::query::F("cat") == "POSIX") &&
                   (dftracer::utils::query::F("dur") > 1000)).build();

   .. tab-item:: C

      .. code-block:: c

         #include <dftracer/utils/query/abi.h>

         dftu_query* a = dftu_query_cmp_str("cat", DFTU_QCMP_EQ, "POSIX");
         dftu_query* b = dftu_query_cmp_i64("dur", DFTU_QCMP_GT, 1000);
         dftu_query* q = dftu_query_and(a, b);   /* consumes a and b */
         /* ... dftu_dataframe_mask(q, ...) ... */
         dftu_query_free(q);

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
   * - SQL LIKE (``%`` any run, ``_`` one char)
     - ``F.name.like("%read%")``, ``F.name.ilike("READ")``
     - ``Field("name").like("%read%")``
   * - regex (ECMAScript)
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

   .. tab-item:: DSL

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
         // C ABI: dftu_query_cmp_str("any(tags)", DFTU_QCMP_EQ, "gpu")

A value that is not an array, an empty array, and elements that are objects or
arrays match nothing. The index skips chunks through the evidence it keeps for
each position (``tags.0``, ``tags.1``, ...), so a membership filter prunes like
a filter on one position; an array wider than 256 positions is scanned. In
memory, ``Expr.apply`` cannot evaluate ``any()``.

Resolved columns
----------------

Traces store hashes, not the full strings, for host, file path, and command.
The index keeps a dictionary per hash kind, and ``resolved.<key field>.<field>``
reads ``field`` of the dictionary row whose key the event's ``key field``
holds. The engine rewrites a filter on it to a key lookup against the index, so
you filter on a readable name without denormalizing the trace. The same names
work in ``select`` and ``group_by``.

.. tab-set::

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils.query import resolved

         q = resolved("fhash.path").like("%/scratch/%") & (resolved("hhash.name") == "node01")

   .. tab-item:: C++

      .. code-block:: cpp

         auto q = (resolved("fhash.path").like("%/scratch/%")
                   && (resolved("hhash.name") == "node01")).build();

The dftracer schema resolves these columns:

.. list-table::
   :header-rows: 1

   * - Column
     - Value
   * - ``resolved.fhash.path``
     - path of the file an I/O event touches
   * - ``resolved.cwd.path``
     - working directory of a process
   * - ``resolved.hhash.name``
     - host name
   * - ``resolved.exec_hash.value``
     - executable of a process
   * - ``resolved.cmd_hash.value``
     - command line of a process

Any other ``resolved.`` name fails with an error that lists these. A filter
that matches more than 4096 keys still returns the right rows, but the index
does not prune with it.

How it runs
-----------

A predicate over indexed fields (``cat``, ``name``, ``pid``, ``ts``, ``dur``,
resolved fields) is **pushed down to the index** at scan time, so only matching
chunks are read. Anything else is evaluated as a **SIMD mask** over the columnar
batch. Either way you write the predicate once; see
:doc:`../../concepts/indexing-and-pushdown` for the pushdown model.
