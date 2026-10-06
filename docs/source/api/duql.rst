:description: Reference for the Python duql builder: queries as Python values that parse to the same tree as duql text, with parameters and terminals.

duql
====

``dftracer.utils.duql`` builds duql queries as Python values. A builder query
parses to the same syntax tree as the equal text, so the two give the same
plan and the same rows, and ``text()`` prints the canonical text. The
language itself is in :doc:`../reference/duql`.

.. code-block:: python

   from dftracer.utils.duql import c, duration, fn, param, source

   q = (
       source("trace.pfw.gz")
       .where((c("cat") == "POSIX") & (c("dur") > duration(1, "ms")))
       .group("name", n=fn.count(), total=c("dur").sum())
       .sort(-c("total"))
       .take(10)
   )
   print(q.text())   # duql 1 / from "trace.pfw.gz" / | where ... on separate lines
   df = q.collect()  # a DataFrame

   slow = source("trace.pfw.gz").where(c("name") == param("n")).bind(n="read")
   slow.count()

Builder calls only build. ``collect``, ``count``, ``first``, ``stream`` and
``explain`` run the query on the file of its first ``source``; ``on(viewer)``
runs it on a ``TraceViewer`` and returns the TraceViewer, for a query with no
source or a viewer you already configured.

Expressions
-----------

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - Builder
     - duql
   * - ``c("args.size")``, ``c("^.run")``
     - a path as written
   * - ``lit(v)``; a plain value beside an operator
     - ``null``, ``true``, a number or a string literal
   * - ``param("n")``
     - ``$n``, bound with ``bind(n=...)``
   * - ``c("xs")[c("i")]`` (``Col.__getitem__``)
     - ``xs[i]``, the element at that row's index
   * - ``duration(250, "ms")``
     - ``250ms``
   * - ``==  !=  <  <=  >  >=  +  -  *  /  //  %``, unary ``-``
     - the same operators
   * - ``a & b``, ``a | b``, ``~a``
     - ``and``, ``or``, ``not``
   * - ``x.coalesce(y)``
     - ``x ?? y``
   * - ``x.is_in([...])``, ``x.not_in(pipe)``, ``x.is_in(param("p"))``
     - ``in`` a list, a sub-query, or a list bound to ``$p``
   * - ``x.between(a, b)``, ``x.not_between(a, b)``
     - ``between``
   * - ``x.like(p, escape=None)``, ``ilike``, ``not_like``, ``not_ilike``
     - ``like`` patterns; ``p`` is a string or ``param("p")``
   * - ``x.regex(p)``, ``iregex``, ``not_regex``, ``not_iregex``
     - ``~``, ``~*``, ``!~``, ``!~*``
   * - ``x.is_null()``, ``is_not_null``, ``is_missing``, ``is_not_missing``
     - ``is [not] null | missing``
   * - ``x.icontains("io", any=False)``
     - ``"io" in x``, or ``"io" in any(x)``
   * - ``x.ref("runs", "app", key=None)``
     - ``x -> runs.app``, or ``x -> runs(key).app``
   * - ``fn.count()``, ``fn.ns.f(x, k=3)``, ``fn("name")(x)``
     - a call; keyword arguments are named arguments
   * - ``x.regex_replace(re, to)``, ``fn("regex_replace")(x, re, to)``
     - ``regex_replace(x, re, to)``
   * - ``x.sum()``, ``x.quantile(0.99)``, any other method
     - a call with ``x`` first: ``sum(x)``, ``quantile(x, 0.99)``
   * - ``tup(a, b)``, ``sub(pipe)``
     - a tuple, a scalar sub-query

An expression has no truth value: ``c("a") == 1 and c("b") == 2`` raises
``TypeError``; write ``&``.

Pipelines
---------

``source(*files)`` reads trace files (``from "a.pfw.gz"``), or the file bound
to a parameter (``source(param("f"))``); ``rowset(name)`` reads a row set or
``let`` (``from data``); ``Pipe()`` reads the data of the viewer that runs it.
Every stage of the language has a method with its name, and each returns a new
``Pipe``: ``where``, ``derive``, ``select``, ``drop``, ``rename``,
``distinct``, ``group``, ``agg``, ``window``, ``pivot``, ``unpivot``,
``sort``, ``take``, ``skip``, ``sample``, ``expand``, ``lookup``,
``lookup_asof``, ``lookup_overlap``, ``union``, ``call``, ``time_range``,
``call_tree``, ``bucket``, ``session`` and ``parse(col, pattern)``. ``let(name, pipe)`` and
``define(name, body, params)`` add declarations before the query. When
``body`` is a ``Pipe``, ``define`` declares a pipeline macro, and
``Pipe.use(name, *args)`` calls it as a stage:
``Pipe().use("io_rate", duration(1, "ms"))``.

``Source()`` builds the duql source of a record schema with
``rowset(name, pipe)``, ``define(name, body, params)`` and
``flag(name, value)``; ``text()`` returns the canonical member text, the
same as the C++ ``duql::Source``.

``Pipe.bucket(width, fill=False, as_=..., mode=..., low=None, high=None,
every=None, at=None)`` adds the ``bucket`` stage. ``every`` makes hopping
windows and ``at`` aligns the starts.

- Keyword arguments name outputs: ``derive(ms=c("dur") / 1000)``,
  ``group("pid", n=fn.count())``. A positional item is a path string or an
  expression; ``expr.alias(name)`` names one.
- Sort keys are paths, expressions, ``-x`` or ``x.desc()`` (descending), and
  ``x.asc(nulls="first")``.
- A lookup key is a path, an expression or a pair ``(this_side,
  rowset_side)`` for ``a == b``.
- ``where`` also takes a columnar filter such as ``F("dur") > 5``.

A string value is written as a literal that reads back as exactly that
string, with escapes (``\"``, ``\\``, ``\n``); ``param`` binds a value without
writing it into the text at all.

Filters with ``F``
------------------

``F`` and ``Field``, exported from ``dftracer.utils``, build columnar filter
expressions that also render to duql (see :doc:`columnar`). They pass to
``TraceViewer.filter`` and to ``Pipe.where``.

.. code-block:: python

   from dftracer.utils import F

   q = (F.cat == "POSIX") & (F.dur > 1000)
   str(q)  # 'cat == "POSIX" and dur > 1000'

Reference
---------

.. automodule:: dftracer.utils.duql
   :members: Pipe, Source, Col, c, lit, param, duration, tup, sub, case_, source, rowset, load_path
