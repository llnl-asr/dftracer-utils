HTTP Server
===========

The ``dftracer_server`` provides a high-performance HTTP server for querying and streaming DFTracer trace data via REST API. It uses bloom filter indexing to accelerate event searches and supports filtering, aggregation, and visualization API endpoints.

Starting the Server
-------------------

Basic startup:

.. code-block:: bash

    dftracer_server -d /path/to/traces

The server scans the trace directory on startup, loads or builds bloom/checkpoint sidecar indexes (``.idx`` files), and begins listening for HTTP requests on ``0.0.0.0:8080``.

Custom Configuration:

.. code-block:: bash

    # Listen on localhost only, port 9000
    dftracer_server -b 127.0.0.1 -p 9000 -d /path/to/traces

    # Use separate index directory (useful for NFS or slow disks)
    dftracer_server -d /path/to/traces --index-dir /var/cache/dftracer_indexes

    # Use 16 worker threads for concurrent request handling
    dftracer_server -d /path/to/traces --executor-threads 16

REST API
--------

All endpoints return JSON responses and support filtering via query parameters. The server uses `HTTP/1.1` with keep-alive connections for efficient streaming.

Trace Data API
~~~~~~~~~~~~~~

GET /api/v1/files
+++++++++++++++++

List all available trace files in the directory.

**Response:**

.. code-block:: json

    {
        "files": [
            {
                "path": "trace1.pfw.gz",
                "has_bloom_data": true,
                "has_checkpoint_index": true,
                "is_small": false
            }
        ],
        "count": 1
    }

GET /api/v1/files/info
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
        "is_small": false,
        "size_mb": 45.2,
        "compressed_size": 47185920,
        "num_lines": 1234567,
        "num_checkpoints": 29,
        "uncompressed_size": 943718400
    }

.. note::

   ``num_lines``, ``num_checkpoints``, and ``uncompressed_size`` are only included for non-small (indexed) files.

GET /api/v1/events
++++++++++++++++++

Query events with optional filtering. Returns results as streaming NDJSON using HTTP/1.1 chunked transfer encoding.

**Query Parameters:**

- ``file`` (string) - Specific trace file to query (default: all files)
- ``limit`` (integer) - Maximum number of events to return, 0-100000 (default: 1000)
- ``cat`` (string) - Category filter (comma-separated for multiple values)
- ``name`` (string) - Event name filter (comma-separated for multiple values)
- ``pid`` (integer) - Process ID filter
- ``ts_min`` (double) - Minimum timestamp in microseconds
- ``ts_max`` (double) - Maximum timestamp in microseconds
- ``dur_min`` (double) - Minimum duration in microseconds
- ``dur_max`` (double) - Maximum duration in microseconds

**Response:** Streaming NDJSON (one JSON object per line)

- ``Content-Type: application/x-ndjson``
- ``Transfer-Encoding: chunked``
- ``X-Limit``: Echoes the limit applied

.. code-block:: text

    {"name":"MPI_Send","ph":"X","ts":1234567890,"dur":12345,"pid":1,"tid":1,"args":{}}
    {"name":"MPI_Recv","ph":"X","ts":1234567950,"dur":200,"pid":1,"tid":1,"args":{}}

**Example:**

.. code-block:: bash

    # Get first 100 MPI_Send events
    curl "http://localhost:8080/api/v1/events?file=trace1.pfw.gz&name=MPI_Send&limit=100"

    # Get events with specific duration threshold
    curl "http://localhost:8080/api/v1/events?file=trace1.pfw.gz&dur_min=1000&limit=50"

    # Filter by category and time range
    curl "http://localhost:8080/api/v1/events?file=trace1.pfw.gz&cat=POSIX&ts_min=1000000&ts_max=2000000"

GET /api/v1/events/stream
+++++++++++++++++++++++++

Stream events as NDJSON without a limit. Identical to ``/api/v1/events`` but ``limit`` defaults to 0 (unlimited). Useful for large result sets and real-time processing.

**Query Parameters:** Same as ``/api/v1/events`` (but ``limit`` defaults to 0)

**Response:** Streaming NDJSON with chunked transfer encoding (same format as ``/api/v1/events``)

.. code-block:: text

    {"name":"MPI_Send","ph":"X","ts":1234567890,"dur":12345,"pid":1,"tid":1,"args":{}}
    {"name":"MPI_Recv","ph":"X","ts":1234567950,"dur":200,"pid":1,"tid":1,"args":{}}

**Example:**

.. code-block:: bash

    # Stream all events with duration > 5000 microseconds
    curl "http://localhost:8080/api/v1/events/stream?file=trace1.pfw.gz&dur_min=5000"

GET /api/v1/stats
+++++++++++++++++

Retrieve aggregated statistics across all trace files. Results are cached in-memory by request path.

**Query Parameters:** None

**Response:**

.. code-block:: json

    {
        "file_count": 3,
        "total_events": 3704701,
        "skipped_small_files": 0,
        "files": [
            {
                "file_path": "trace1.pfw.gz",
                "success": true,
                "total_events": 1234567,
                "num_categories": 5,
                "num_unique_names": 42,
                "num_pid_tids": 8,
                "time_range": {
                    "min_timestamp_us": 1000000,
                    "max_timestamp_us": 5000000,
                    "time_span_seconds": 4.0
                },
                "duration": {
                    "count": 1234567,
                    "sum_us": 98765432,
                    "mean_us": 80.0,
                    "stddev_us": 15.2,
                    "min_us": 1,
                    "max_us": 45000
                },
                "category_counts": {"POSIX": 500000, "APP": 734567},
                "name_counts": {"read": 250000, "write": 250000},
                "pid_tid_counts": {"1:1": 600000, "1:2": 634567}
            }
        ]
    }

GET /api/v1/info
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
                "is_small": false,
                "min_timestamp_us": 1000000,
                "max_timestamp_us": 5000000
            },
            {
                "path": "trace2.pfw.gz",
                "has_bloom_data": true,
                "has_checkpoint_index": true,
                "is_small": false,
                "min_timestamp_us": 3000000,
                "max_timestamp_us": 7000000
            },
            {
                "path": "trace3.pfw.gz",
                "has_bloom_data": true,
                "has_checkpoint_index": true,
                "is_small": false,
                "min_timestamp_us": 5000000,
                "max_timestamp_us": 10000000
            }
        ]
    }

Visualization API
~~~~~~~~~~~~~~~~~

GET /api/v1/viz/events
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
    curl "http://localhost:8080/api/v1/viz/events?begin=0&end=1000000&summary=1&lanes=%5B%7B%22field%22%3A%22pid%22%2C%22value%22%3A%221%22%7D%5D"

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
    curl "http://localhost:8080/api/v1/viz/events?begin=0&end=1000000&summary=1"

    # Same query with raw timestamps and PID filter
    curl "http://localhost:8080/api/v1/viz/events?begin=1000000&end=2000000&summary=1&ts_normalize=0&pid=1"

Event Filtering
---------------

The trace data endpoints (``/api/v1/events``, ``/api/v1/events/stream``) use structured query parameters for filtering:

**Query Parameter Filters:**

- ``cat`` - Filter by category (comma-separated for multiple: ``cat=POSIX,STDIO``)
- ``name`` - Filter by event name (comma-separated for multiple: ``name=read,write``)
- ``pid`` - Filter by process ID
- ``ts_min`` / ``ts_max`` - Filter by timestamp range (microseconds)
- ``dur_min`` / ``dur_max`` - Filter by duration range (microseconds)

.. code-block:: bash

    # Get POSIX read/write events with duration > 1000 us
    curl "http://localhost:8080/api/v1/events?file=trace.pfw.gz&cat=POSIX&name=read,write&dur_min=1000"

    # Get events in a time window
    curl "http://localhost:8080/api/v1/events?file=trace.pfw.gz&ts_min=1000000&ts_max=2000000"

**Visualization Filters:**

The ``/api/v1/viz/events`` endpoint supports JSON-based ``filters`` for complex predicates:

.. code-block:: json

    [
        {"field": "pid", "op": "=", "value": 1},
        {"field": "dur", "op": ">=", "value": 500}
    ]

Supported operators: ``=``, ``>=``, ``<=``, ``>``, ``<``

Indexing
--------

**Bloom Filters:**

Trace files larger than 1 MB (compressed) are automatically indexed with bloom filters (``.idx`` files) during server startup. Bloom filters accelerate event filtering by skipping chunks that cannot contain matching events.

**Checkpoint Indexes:**

Checkpoint indexes (``.idx`` files) store byte offsets and decompression state, enabling efficient random access to events by line number or byte position.

**Small Files:**

Files smaller than 1 MB are streamed directly without sidecar indexes. The server detects this automatically based on compressed file size.

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

Event filtering streams through bloom indexes and partial reads, minimizing memory usage. Both ``/api/v1/events`` and ``/api/v1/events/stream`` use chunked transfer encoding with iovec scatter-gather I/O, streaming NDJSON results without buffering the full response in memory.

**Query Optimization:**

- Use narrow time ranges in ``/api/v1/viz/events`` queries
- Apply filters (``cat``, ``name``, ``pid``, time/duration ranges) to reduce the number of events scanned
- Use ``limit`` for pagination on ``/api/v1/events``
- Use higher ``summary`` levels in visualization queries to aggregate short-duration events
- Consider ``lanes`` filtering for visualization queries to reduce network overhead
