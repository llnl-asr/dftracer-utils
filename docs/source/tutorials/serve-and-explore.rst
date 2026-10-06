:description: Start dftracer_server over a trace directory and query it with plain curl - no Python or C++ compiler needed.

Serve a trace and explore it over HTTP
=======================================

By the end of this lesson you will have started ``dftracer_server`` over a
directory of ``.pfw.gz`` traces and queried it with plain ``curl``, with no
Python, no C++ compiler. It assumes :doc:`first-analysis`; the server answers
the same kind of query, over HTTP instead of a library call.

1. Create a trace to serve
----------------------------

The server serves a **directory**, so put a trace inside one:

.. code-block:: python

   import gzip, os

   os.makedirs("traces", exist_ok=True)
   with gzip.open("traces/trace-0.pfw.gz", "wt") as f:
       for i in range(500):
           cat = "POSIX" if i % 2 else "STDIO"
           f.write(
               f'{{"name":"read","cat":"{cat}","pid":1,"tid":1,'
               f'"ts":{1000 + i},"dur":{10 + i},"ph":"X","args":{{}}}}\n'
           )

2. Start the server
---------------------

``dftracer_server`` scans ``-d``/``--directory`` on startup, builds any
missing ``.dftindex`` sidecars, and listens on ``127.0.0.1:8080`` by default:

.. code-block:: console

   $ dftracer_server -d ./traces -p 8099
   Using trace directory for indexes: ./traces
   ...
   DFTracer server listening on 127.0.0.1:8099
   Trace viewer UI: http://127.0.0.1:8099/
   Serving 1 trace files from ./traces

Leave it running in this terminal (or background it) and query it from
another. See :doc:`../guides/serving/http-server` for the full flag reference,
including ``--bind``/``--token`` for exposing it beyond loopback.

3. Ask the global summary and file list
------------------------------------------

Every route returns one JSON response. Start with ``/api/info`` and
``/api/files``:

.. code-block:: console

   $ curl 'http://127.0.0.1:8099/api/info'
   {"file_count":1,"schema":{"id":"dftracer","decoder":"dftracer","fields":{
    "time":"ts","duration":"dur","entity":"pid","lane":"tid","label":"name",
    "entity_name":"pid"}},"time_range":{"min_timestamp_us":1000,
    "max_timestamp_us":2008},"files":[{"path":"./traces/trace-0.pfw.gz","has_bloom_data":true,
    "has_checkpoint_index":true,"min_timestamp_us":1000,"max_timestamp_us":2008}]}

   $ curl 'http://127.0.0.1:8099/api/files'
   {"files":[{"path":"./traces/trace-0.pfw.gz","has_bloom_data":true,
    "has_checkpoint_index":true}],"count":1}

The trace's timestamps run from 1000 to 2008 (microseconds); you will use that
window next. ``schema`` names the record schema the server read the trace with
and the field that plays each role. The server serves any JSON trace through
its schema, so the viewer's time, lane and name fields follow it.

4. Fetch events for a timeline
---------------------------------

``/api/viz/events`` is the workhorse behind the trace viewer's timeline:
time-windowed, level-of-detail-aggregated events. ``begin``, ``end``, and
``summary`` are required; ``summary=1`` is full detail. Give it a window that
actually covers the trace's short duration. A huge ``end`` (like
``999999999``) makes the server compute a coarse duration floor sized for that
whole span, which drops every sub-millisecond event in a demo this small:

.. code-block:: console

   $ curl 'http://127.0.0.1:8099/api/viz/events?begin=0&end=1600&summary=1' | head -c 300
   {"events":[{"name":"read","cat":"STDIO","pid":1,"tid":1,"ts":0,"dur":10,
   "ph":"X","args":{}},{"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1,
   "dur":11,"ph":"X","args":{}}, ...

The response's ``metadata.count`` is 500 (every event in the window);
``ts`` is normalized to start at 0 (``ts_normalized:true``), and
``metadata.global_min_timestamp_us`` (1000) is the value that was subtracted.
Narrow it with ``cat`` or a ``duql`` predicate:

.. code-block:: console

   $ curl -s 'http://127.0.0.1:8099/api/viz/events?begin=0&end=1600&summary=1&cat=POSIX' \
       | python3 -c 'import json,sys; print(json.load(sys.stdin)["metadata"])'
   {'begin': 0.0, 'end': 1600.0, 'count': 250, 'limit': 0, 'truncated': False,
    'ts_normalized': True, 'global_min_timestamp_us': 1000}

   $ curl -s 'http://127.0.0.1:8099/api/viz/events?begin=0&end=1600&summary=1&duql=dur%20%3E%3D%20400' \
       | python3 -c 'import json,sys; print(json.load(sys.stdin)["metadata"]["count"])'
   110

5. Ask for a per-name summary
--------------------------------

``/api/viz/stats`` aggregates by event name over the same time window. It is the
data behind the trace viewer's Analyze tab:

.. code-block:: console

   $ curl 'http://127.0.0.1:8099/api/viz/stats?begin=0&end=1600&summary=1'
   {"count":500,"total_dur":129750.0,"wall":1600.0,"truncated":false,
    "names":[{"name":"read","count":500,"total":129750.0,"avg":259.5,
    "min":10.0,"max":509.0,"coverage":0.0}]}

What you learned
------------------

- ``dftracer_server -d <dir> -p <port>`` serves a trace directory over HTTP,
  building missing indexes on startup.
- Every ``/api/*`` route returns one JSON response; ``/api/info`` and
  ``/api/files`` need no parameters.
- ``/api/viz/events`` and ``/api/viz/stats`` take a required ``begin``/``end``
  microsecond window and a ``summary`` level-of-detail knob, and accept
  ``cat``/``duql`` filters.

See also
----------

- :doc:`../guides/serving/http-server`, every flag, the access token, and the
  full data-route table (``/api/files/info``, ``/api/rowset``,
  ``/api/cancel``).
- :doc:`../guides/serving/viz-api`, every ``/api/viz/*`` route, its extra
  parameters, and the level-of-detail model behind ``summary``.
- :doc:`../trace-viewer`, the web UI the same server hosts at ``/``.
