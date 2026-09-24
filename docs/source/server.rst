:description: Run dftracer_server: a REST API over bloom-filter-indexed traces for filtering, aggregation, and visualization, plus the bundled web UI.

HTTP Server
===========

The ``dftracer_server`` provides a high-performance HTTP server for querying DFTracer trace data via a REST API. It uses bloom filter indexing to accelerate event searches and supports filtering, aggregation, and visualization API endpoints.

Starting the Server
-------------------

Basic startup:

.. code-block:: bash

    dftracer_server -d /path/to/traces

The server scans the trace directory on startup, loads or builds a bloom/checkpoint sidecar index (a ``.dftindex`` directory), and begins listening for HTTP requests on ``127.0.0.1:8080``. It also serves the interactive :doc:`trace-viewer` web UI at ``/`` and ``/index.html``.

Custom Configuration:

.. code-block:: bash

    # Expose on all interfaces (see Security below), port 9000
    dftracer_server -b 0.0.0.0 -p 9000 -d /path/to/traces

    # Use separate index directory (useful for NFS or slow disks)
    dftracer_server -d /path/to/traces --index-dir /var/cache/dftracer_indexes

    # Use 16 worker threads for concurrent request handling
    dftracer_server -d /path/to/traces --executor-threads 16

    # Auto-shutdown after 30 minutes of uptime (0, the default, disables it)
    dftracer_server -d /path/to/traces --timeout 30m

``--timeout`` bounds the server's total uptime and then triggers a graceful
shutdown; it accepts a humanized duration (``30m``, ``1.5h``, ``0`` to
disable). Use it to stop lingering processes when a client such as the VSCode
extension closes without killing the server.

Security
--------

The server binds to loopback (``127.0.0.1``) by default, so it is not reachable
from other hosts unless you explicitly pass ``-b 0.0.0.0`` (or another
address). When exposing it beyond localhost, set an access token:

.. code-block:: bash

    dftracer_server -d /path/to/traces -b 0.0.0.0 --token "$(openssl rand -hex 16)"

With ``--token`` set, every request must present the token, either as a
``?token=<TOKEN>`` query parameter or an ``Authorization: Bearer <TOKEN>``
header; otherwise the server responds ``401 Unauthorized``. Responses carry an
open CORS header (``Access-Control-Allow-Origin: *``) so browser and editor
webview clients can query the API cross-origin.

REST API
--------

All endpoints return a single JSON response and support filtering via query parameters. The server uses `HTTP/1.1` with keep-alive connections.

Trace Data API
~~~~~~~~~~~~~~

GET /api/files
+++++++++++++++++

List all indexed trace files.

**Response:**

.. code-block:: json

    {
        "files": [
            {
                "path": "trace1.pfw.gz",
                "has_bloom_data": true,
                "has_checkpoint_index": true
            }
        ],
        "count": 1
    }

GET /api/files/info
++++++++++++++++++++++

Get detailed metadata for a specific file.

**Query Parameters:**

- ``file`` (string) - Path to the trace file (e.g., ``trace1.pfw.gz``) [required]

**Response:**

.. code-block:: json

    {
        "path": "trace1.pfw.gz",
        "has_bloom_data": true,
        "has_checkpoint_index": true,
        "size_mb": 45.2,
        "compressed_size": 47185920,
        "num_lines": 1234567,
        "num_checkpoints": 29,
        "uncompressed_size": 943718400
    }

GET /api/info
++++++++++++++++

Get global metadata about all trace files (time bounds, file listing).

**Response:**

.. code-block:: json

    {
        "file_count": 3,
        "time_range": {
            "min_timestamp_us": 1000000,
            "max_timestamp_us": 10000000
        },
        "files": [
            {
                "path": "trace1.pfw.gz",
                "has_bloom_data": true,
                "has_checkpoint_index": true,
                "min_timestamp_us": 1000000,
                "max_timestamp_us": 5000000
            },
            {
                "path": "trace2.pfw.gz",
                "has_bloom_data": true,
                "has_checkpoint_index": true,
                "min_timestamp_us": 3000000,
                "max_timestamp_us": 7000000
            }
        ]
    }

GET /api/resolve
++++++++++++++++++

Resolve dictionary keys (file/host/string hashes) to their values.

**Query Parameters:**

- ``hash`` (string) - One key, or several separated by commas [required]
- ``type`` (string) - the dictionary: ``file`` (default), ``host``, or
  ``string``
- ``field`` (string) - the dictionary field: ``path``, ``name`` or ``value``;
  the dictionary's own field by default

**Response:**

.. code-block:: json

    {"names": {"314c1a1cdb22a136": "/data/train/img_0.npz"}}

Unknown hashes are simply absent from the reply.

POST /api/cancel
++++++++++++++++++

Cancel an in-flight request by its ``X-Request-Id`` header. Every request the
router dispatches is registered under the ``X-Request-Id`` it was sent with (if
any); posting its id here interrupts the in-flight scan cooperatively.

**Query Parameters:**

- ``id`` (string) - The request id to cancel [required]

**Response:**

.. code-block:: json

    {"cancelled": true}

Time units
++++++++++

All timestamps and durations in the server API are **microseconds**. If a trace
declares a different unit via its ``CM`` ``time_metric`` metadata event
(``NS``/``MS``/``SEC``; absent means ``US``), the server resolves that unit once
per trace and converts every request bound and response value to microseconds -
so clients always work in microseconds regardless of the trace's native unit.

Visualization API
~~~~~~~~~~~~~~~~~

GET /api/viz/events
++++++++++++++++++++++

Query events optimized for visualization with time-range windowing, lane grouping, and summary aggregation. Returns events binned and aggregated for efficient rendering in trace viewers.

**Query Parameters:**

- ``begin`` (double) - Time window start [required] (normalized if ``ts_normalize=1``)
- ``end`` (double) - Time window end [required] (normalized if ``ts_normalize=1``)
- ``summary`` (integer) - Summary level [required] (1=full detail, higher=aggregate shorter events)
- ``file`` (string) - Specific trace file to query (default: all files)
- ``ts_normalize`` (integer) - If 1 (default), normalize timestamps relative to global minimum; if 0, use raw timestamps
- ``limit`` (integer) - Maximum events to return, 0=unlimited (default: 0)
- ``pid`` (integer) - Process ID filter
- ``tid`` (integer) - Thread ID filter
- ``cat`` (string) - Category filter
- ``lanes`` (JSON) - Lane filtering as URL-encoded JSON (see below)
- ``filters`` (JSON) - Complex filtering as URL-encoded JSON array (see below)
- ``query`` (string) - A raw query DSL predicate, ANDed with the other filters

**Response:**

.. code-block:: json

    {
        "events": [
            {
                "name": "MPI_Send",
                "ph": "X",
                "ts": 234567,
                "dur": 12345,
                "pid": 1,
                "tid": 1,
                "args": {}
            }
        ],
        "metadata": {
            "begin": 0,
            "end": 1000000,
            "count": 1,
            "limit": 0,
            "truncated": false,
            "ts_normalized": true,
            "global_min_timestamp_us": 1000000
        }
    }

**Timestamp Normalization:**

When ``ts_normalize=1`` (default), the client sends begin/end values relative to the global minimum timestamp. The server de-normalizes internally for queries and normalizes the response events. The ``global_min_timestamp_us`` field is included for client-side denormalization if needed.

**Summary Level:**

The summary level controls a minimum duration threshold: ``time_range / (viewport_width * summary_level)`` (viewport_width defaults to 1920). Events below this threshold are filtered out at higher summary levels.

**Lane Filtering:**

Filter events by lane using URL-encoded JSON:

.. code-block:: bash

    # Filter by process ID
    curl "http://localhost:8080/api/viz/events?begin=0&end=1000000&summary=1&lanes=%5B%7B%22field%22%3A%22pid%22%2C%22value%22%3A%221%22%7D%5D"

The ``lanes`` parameter accepts a JSON array or object:

.. code-block:: json

    [{"field": "pid", "value": "1"}]

**Complex Filtering:**

The ``filters`` parameter accepts a URL-encoded JSON array with field/operator/value objects:

.. code-block:: json

    [
        {"field": "pid", "op": "=", "value": 1},
        {"field": "dur", "op": ">=", "value": 0}
    ]

Supported operators: ``=``, ``>=``, ``<=``, ``>``, ``<``

**Example:**

.. code-block:: bash

    # Get events for visualization in normalized time range [0, 1M]
    curl "http://localhost:8080/api/viz/events?begin=0&end=1000000&summary=1"

    # Same query with raw timestamps and PID filter
    curl "http://localhost:8080/api/viz/events?begin=1000000&end=2000000&summary=1&ts_normalize=0&pid=1"

GET /api/viz/untimed
++++++++++++++++++++++

Records written without a clock (``ts`` 0, such as CUDA activity): events and
aggregated records. The timeline starts at the first start above 0, so it
cannot place them; the viewer counts them in its status bar and pages through
them in the "no time" tab.

**Query Parameters:**

- ``query`` (string) - DSL predicate, as for ``/api/viz/events``
- ``offset`` (integer) - First record of the page (default: 0)
- ``limit`` (integer) - Records per page, at most 10000 (default: 1000)

**Response:** ``count`` covers every matching record; ``events`` is the page,
longest first, with ``dur`` in microseconds and ``ph`` 1 for an event or 3 for
an aggregated record.

.. code-block:: json

    {"events": [{"name": "cudaLaunchKernel", "cat": "CUDA", "pid": 1,
                 "tid": 2, "dur": 7, "ph": 1}],
     "count": 3, "offset": 0, "limit": 1000}

GET /api/viz/breaks
++++++++++++++++++++++

Globally idle time gaps, and multi-run detection (several distinct app runs
back-to-back in one trace).

**Query Parameters:**

- ``ts_normalize`` (integer) - Normalize to the global minimum (default: 1)

.. code-block:: bash

    curl "http://localhost:8080/api/viz/breaks"

.. code-block:: json

    {"gaps": [{"begin": 50000, "end": 900000}], "multi_run": true}

GET /api/viz/density
+++++++++++++++++++++++

Like ``/api/viz/events``, but instead of dropping sub-pixel events it buckets
them per ``(pid, tid, pixel-column)`` into aggregated *density* blocks, so
zoomed-out views still show where activity is. Returns full-size events (with
``args``, for the detail panel) plus a ``density`` array of blocks. Same
``begin``/``end``/``summary`` parameters as ``/api/viz/events``.

Optional ``group_by=<column>`` splits blocks by an event column (a top-level
field, an ``args`` key, or a resolved column such as ``resolved.fhash.path``).
Each block gains a ``group`` value; dictionary key columns keep the raw key and
the response metadata carries a ``group_names`` map (key to value). Events
missing the column, or a column that does not exist, group under ``(none)`` on
the client - they are never dropped.

.. code-block:: bash

    curl "http://localhost:8080/api/viz/density?begin=0&end=999999999&summary=2"
    curl "http://localhost:8080/api/viz/density?begin=0&end=999999999&summary=2&group_by=cat"

.. code-block:: json

    {
      "events": [],
      "density": [
        {"pid": 100, "tid": 100, "ts": 0, "dur": 24457,
         "count": 910, "total": 22044, "depth": 0}
      ]
    }

GET /api/viz/columns
+++++++++++++++++++++++

The complete set of groupable columns in the trace (top-level scalar fields plus
``args`` keys), for the lane-grouping UI. Read from the index's path catalog,
with the summary scan as fallback for older indexes;
``ready`` is ``false`` while that fallback scan is still running.

.. code-block:: bash

    curl "http://localhost:8080/api/viz/columns"

.. code-block:: json

    {"columns": ["cat", "name", "mhost", "fhash"], "ready": true}

GET /api/viz/counters
++++++++++++++++++++++++

Per-bucket read/write bytes and I/O operation counts over a time range, for the
bandwidth / IOPS tracks. Parameters ``begin``, ``end``, ``summary``; returns a
``buckets`` array. Aggregated server-side in parallel.

.. code-block:: bash

    curl "http://localhost:8080/api/viz/counters?begin=0&end=999999999&summary=1"

.. code-block:: json

    {"buckets": [{"ts": 0, "read_bytes": 4096, "write_bytes": 0,
                  "read_ops": 1, "write_ops": 0}]}

GET /api/viz/stats
+++++++++++++++++++++

Server-side per-name aggregation over a time range (the Analyze panel). Returns
a ``names`` table with ``count``, ``total``, ``avg``, ``min``, ``max`` per
operation name. Parameters ``begin``, ``end``, ``summary``; whole-trace,
unfiltered queries are answered from a prebuilt summary.

.. code-block:: bash

    curl "http://localhost:8080/api/viz/stats?begin=0&end=999999999&summary=1"

.. code-block:: json

    {
      "count": 100, "total_dur": 5000,
      "names": [{"name": "read", "count": 50, "total": 2500,
                 "avg": 50, "min": 10, "max": 90}]
    }

GET /api/viz/histogram
+++++++++++++++++++++++++

Distribution of event durations matching the query in ``[begin, end]``: exact
percentiles (``p50``, ``p99``, ...) plus a log-spaced ``buckets`` histogram of
the shape. Narrow to one operation by folding ``name == "..."`` into the query.

.. code-block:: bash

    curl "http://localhost:8080/api/viz/histogram?begin=0&end=999999999&summary=1"

.. code-block:: json

    {"min": 10, "max": 900, "p50": 150, "p99": 880, "buckets": []}

GET /api/viz/proctree
++++++++++++++++++++++++

Infers the process fork hierarchy and returns a ``nodes`` array (one per
process) with parent links, spawn/first timestamps, resolved ``host``, ``rank``
(from ``PR`` metadata), and per-process ``bytes`` / ``io_ops`` / ``io_busy``.
Drives lane ordering and grouping in the viewer. Respects ``?file=`` for
per-node trees on multi-node traces.

.. code-block:: bash

    curl "http://localhost:8080/api/viz/proctree"

.. code-block:: json

    {"nodes": [{"pid": 100, "parent": -1, "host": "node01", "rank": "0",
                "bytes": 16384, "io_ops": 4, "io_busy": 600.0}]}

GET /api/viz/calltree
++++++++++++++++++++++++

Merges events into a flamegraph tree from ``ts``/``dur`` containment; identical
name-paths fold together. Each node carries inclusive ``total``, exclusive
``self``, ``count``, and ``children``. Parameters ``begin``, ``end``,
``summary``; ``group=pid`` keeps each process's tree separate.

.. code-block:: bash

    curl "http://localhost:8080/api/viz/calltree?begin=0&end=999999999&summary=1"

.. code-block:: json

    {
      "name": "root", "total": 5000, "self": 0, "count": 0,
      "children": [{"name": "read", "total": 2500, "self": 2500, "count": 50}]
    }

GET /api/viz/layers
++++++++++++++++++++++

Whole-trace reference data: the operation-name to category ``layers`` map (a
property of the name, so fetched once), plus ``total_files`` (declared via
``FH`` metadata) and ``io_files`` (those an I/O event actually touched).

.. code-block:: bash

    curl "http://localhost:8080/api/viz/layers"

.. code-block:: json

    {"layers": {"read": "POSIX", "write": "POSIX"},
     "total_files": 2, "io_files": 2}

GET / , /api, and /api/openapi.json
+++++++++++++++++++++++++++++++++++

``GET /`` (and ``/index.html``) serve the embedded :doc:`trace-viewer` web UI.
``GET /api`` serves the interactive API explorer page, and ``GET
/api/openapi.json`` returns the OpenAPI 3.1 specification for every endpoint
above (generated from the server's own route table), so external tools can
consume it too. None of these pages contain trace data; they query the API.

Event Filtering
---------------

``GET /api/viz/events`` (and the other ``/api/viz/*`` endpoints that scan
events) filter through a query built server-side from several sources, all
ANDed together:

- ``pid`` / ``tid`` / ``cat`` - equality filters on the top-level fields.
- ``lanes`` - a URL-encoded JSON array/object selecting lanes, e.g.
  ``[{"field": "pid", "value": "1"}]``.
- ``filters`` - a URL-encoded JSON array of field/operator/value predicates:

  .. code-block:: json

      [
          {"field": "pid", "op": "=", "value": 1},
          {"field": "dur", "op": ">=", "value": 500}
      ]

  Supported operators: ``=``, ``>=``, ``<=``, ``>``, ``<``.
- ``query`` - a raw query DSL predicate, e.g. ``dur >= 1000 and cat ==
  "POSIX"``, spliced in verbatim. This is what the timeline's query box sends.

.. code-block:: bash

    curl "http://localhost:8080/api/viz/events?begin=0&end=2000000&summary=1&cat=POSIX"
    curl "http://localhost:8080/api/viz/events?begin=0&end=2000000&summary=1&query=dur%20%3E%3D%201000"

Use ``GET /api/resolve`` to turn an interned hash (``fhash``, ``hhash``, a
file or host id from a response) back into its string.

Indexing
--------

**Bloom Filters:**

Every trace file without an existing sidecar index is automatically indexed with bloom filters during server startup, regardless of file size. Bloom filters accelerate event filtering by skipping chunks that cannot contain matching events.

**Checkpoint Indexes:**

Checkpoint indexes store byte offsets and decompression state, enabling efficient random access to events by line number or byte position.

**Index Persistence:**

Sidecar index files are stored in the trace directory (or ``--index-dir`` if specified) and persist across server restarts. Rebuilding indexes on subsequent starts is avoided, improving startup time.

Error Handling
--------------

The server returns standard HTTP status codes:

- ``200 OK`` - Request succeeded
- ``400 Bad Request`` - Invalid query parameter or filter syntax
- ``404 Not Found`` - Requested file or endpoint does not exist
- ``500 Internal Server Error`` - Unexpected server error (check logs)

Error responses include a JSON error message:

.. code-block:: json

    {
        "error": "File not found: trace.pfw.gz"
    }

Performance Considerations
--------------------------

**Concurrency:**

The server uses coroutine-based concurrency to handle multiple simultaneous requests efficiently. Adjust ``--executor-threads`` to match your CPU core count for best throughput.

**Memory:**

Event filtering streams through bloom indexes and partial reads, minimizing
memory usage during the scan. The response itself is a single JSON body
(built once the scan completes), not a chunked/streamed one.

**Client Receive Timeouts:**

A ``/api/viz/*`` request can hold its connection open while the server is
still scanning chunks before any response bytes are written - there is no
progress signal until the full JSON body is ready. Clients should set a
receive timeout of at least **15 seconds** (the timeout used by the bundled
integration tests, raised from 2 s in earlier builds) to accommodate the
worst-case index-warmup path; the server itself does not impose a global
request timeout (``with_global_timeout(0)``). Cancel a slow request early with
``POST /api/cancel?id=<X-Request-Id>``.

**Query Optimization:**

- Use narrow time ranges in ``/api/viz/events`` queries.
- Apply filters (``cat``, ``pid``, ``tid``, ``query``) to reduce the number of events scanned.
- Use ``limit`` for pagination on ``/api/viz/events``.
- Use higher ``summary`` levels in visualization queries to aggregate short-duration events.
- Consider ``lanes`` filtering for visualization queries to reduce network overhead.
