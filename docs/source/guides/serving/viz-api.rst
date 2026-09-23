:description: Drive the /api/viz/* routes directly to build your own timeline, density plot, or flamegraph over a served trace directory.

Query the visualization API
============================

.. admonition:: Goal
   :class: goal

   Drive the ``/api/viz/*`` routes directly, the way the bundled
   :doc:`../../trace-viewer` does, to build your own timeline, density plot, or
   flamegraph over a served trace directory. This guide assumes
   ``dftracer_server`` is already running; see :doc:`http-server` for how to
   start it, the shared ``/api/*`` data routes, and the access token.

Every route below returns one JSON response and takes ``begin``/``end`` as a
required microsecond time window (a request missing either one gets a
``400 Bad Request``); pass ``0``/``999999999`` to mean "the whole trace" in
practice. ``summary`` is the level-of-detail knob: ``1`` is full detail, and
higher values apply a coarser duration floor so a zoomed-out view returns
fewer, larger events instead of every sub-pixel one.

Fetch events for a timeline
----------------------------

``/api/viz/events`` is the workhorse: time-windowed, LOD-aggregated Chrome
Trace Event JSON for rendering a timeline.

.. code-block:: bash

   curl 'http://127.0.0.1:8080/api/viz/events?begin=0&end=1000000&summary=1'
   # {"events":[...],"metadata":{"begin":0,"end":1000000,"count":42,
   #  "limit":0,"truncated":false,"ts_normalized":true,
   #  "global_min_timestamp_us":1000000}}

``begin``/``end``/``summary`` are required. Narrow the result with:

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Param
     - Meaning
   * - ``pid``, ``tid``
     - Keep only events on this process / thread id.
   * - ``cat``
     - Keep only events in this category.
   * - ``file``
     - Scan only this one indexed file (by path), instead of every file.
   * - ``query``
     - A :doc:`../core/query-dsl` predicate, e.g. ``dur >= 1000``, ANDed onto
       the other filters.
   * - ``lanes``
     - A JSON array of ``{"field": ..., "value": ...}`` objects (or one such
       object), each turned into a ``field == value`` clause. This is how the
       viewer's per-lane selection narrows a request.
   * - ``filters``
     - A JSON array of ``{"field": ..., "op": ..., "value": ...}`` objects.
       ``op`` is one of ``=``, ``>=``, ``<=``, ``>``, ``<``. ``begin``/``end``
       map to the ``ts`` column and ``duration`` maps to ``dur``.
   * - ``limit``
     - Cap the number of events returned (``0`` = unlimited). The response
       marks ``truncated: true`` when the cap was hit.
   * - ``width``
     - Client canvas width in px; sets the sub-pixel duration cutoff used at
       ``summary=1`` so a wide unfiltered window does not return millions of
       events that could not be drawn anyway. Default ``1920``.
   * - ``lookback``
     - How far (us) to look behind ``begin`` for events that started earlier
       but extend into the window. Default ``0``.
   * - ``ts_normalize``
     - ``1`` (default) sends/receives timestamps relative to the trace's
       global minimum; ``0`` uses absolute timestamps.

``lanes`` and ``filters`` both compile down to the same query-DSL predicate
that ``query`` accepts; pass whichever shape is convenient for your client -
they combine with AND.

Other visualization routes
---------------------------

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - Path
     - Description and extra params
   * - ``/api/viz/density``
     - Sub-pixel events folded into density blocks for a minimap or overview
       track. Extra: ``width`` (default ``1920``, sets the fold cutoff),
       ``group_by`` (comma-separated column names to key blocks by, e.g.
       ``pid,tid``). ``summary`` defaults to ``2`` here (vs. ``1`` for
       ``/events``). Same ``query``/``lanes``/``filters``/``file``/
       ``ts_normalize`` as ``/events``.
   * - ``/api/viz/breaks``
     - Globally-idle time gaps and multi-run detection, for drawing "run"
       boundaries. Param: ``ts_normalize`` (default ``1``). No time window
       needed; it looks at the whole trace.
   * - ``/api/viz/counters``
     - Read/write bytes and I/O op counts per time bucket (``ph="C"``
       events). Extra: ``buckets`` (target bucket count, default ``800``,
       clamped to ``[16, 4000]``), ``query``.
   * - ``/api/viz/proctree``
     - Inferred process/fork hierarchy with host, rank, and I/O per process.
       Param: ``file`` (limit to one trace file), ``ts_normalize``. No time
       window; it summarizes the whole trace.
   * - ``/api/viz/columns``
     - The set of groupable columns present in the trace (for populating a
       "group by" dropdown). No params.
   * - ``/api/viz/stats``
     - Per-name (or per-``group``) aggregation over ``[begin, end]`` - count,
       total/avg/min/max duration, and wall-time coverage. Extra: ``group``
       (``name`` default, or ``cat``/``pid``/``fhash``/``file``), plus the
       same ``query``/``lanes``/``filters``/``file``/``pid``/``tid`` filters
       as ``/events``.
   * - ``/api/viz/calltree``
     - A merged flamegraph tree built from ts/dur containment across the
       window. Extra: ``group`` (set to ``pid`` to keep processes separate
       instead of folding them together), ``limit`` (cap events folded in).
   * - ``/api/viz/histogram``
     - The duration distribution of matching events: percentiles (p50/p90/
       p95/p99) and a log-spaced bucket histogram. Extra: ``buckets``
       (bucket count, default ``40``, clamped to ``[4, 200]``), ``query``.
   * - ``/api/viz/layers``
     - The whole-trace operation-name to category map, plus declared vs.
       I/O-touched file counts. No params.

A couple of worked requests
-----------------------------

Per-category duration breakdown over a window:

.. code-block:: bash

   curl 'http://127.0.0.1:8080/api/viz/stats?begin=0&end=1000000&group=cat'
   # {"count":100,"total_dur":5000,"wall":1000000,"truncated":false,
   #  "names":[{"name":"posix","count":50,"total":2500,"avg":50,
   #            "min":10,"max":90,"coverage":480000}]}

Narrow to one lane and one field filter with ``lanes``/``filters`` (URL-encode
the JSON in a real client; ``curl -G --data-urlencode`` does it here):

.. code-block:: bash

   curl -G 'http://127.0.0.1:8080/api/viz/events' \
     --data-urlencode 'begin=0' \
     --data-urlencode 'end=1000000' \
     --data-urlencode 'summary=1' \
     --data-urlencode 'lanes=[{"field":"pid","value":100}]' \
     --data-urlencode 'filters=[{"field":"duration","op":">=","value":1000}]'

A flamegraph, grouped by process:

.. code-block:: bash

   curl 'http://127.0.0.1:8080/api/viz/calltree?begin=0&end=1000000&group=pid'
   # {"truncated":false,"tree":{"name":"all","total":5000,"self":0,
   #  "count":0,"children":[{"name":"read","total":2500,"self":2500,
   #  "count":50,"children":[]}]}}

Requests are cancellable and cacheable the same way as any other route; see
:doc:`http-server` for the ``X-Request-Id`` / ``/api/cancel`` flow and the
access token.

See also
--------

- :doc:`http-server` - starting the server, the shared ``/api/*`` data
  routes, and request cancellation.
- :doc:`../core/query-dsl` - the predicate language accepted by ``query``,
  and what ``lanes``/``filters`` compile into.
- :doc:`../../trace-viewer` - the bundled web UI built on these routes.
