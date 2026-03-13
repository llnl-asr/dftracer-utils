HTTP Server
===========

The ``dftracer_server`` provides a high-performance HTTP server for querying and streaming DFTracer trace data via REST API. It uses bloom filter indexing to accelerate event searches and supports filtering, aggregation, and visualization API endpoints.

Starting the Server
-------------------

Basic startup:

.. code-block:: bash

    dftracer_server -d /path/to/traces

The server scans the trace directory on startup, loads or builds bloom/checkpoint sidecar indexes (``.bidx`` and ``.idx`` files), and begins listening for HTTP requests on ``0.0.0.0:8080``.

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
                "size_mb": 45.2,
                "num_lines": 1234567,
                "min_timestamp_us": 1000000,
                "max_timestamp_us": 5000000,
                "has_bloom_index": true,
                "has_checkpoint_index": true,
                "is_small": false
            }
        ]
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
        "size_mb": 45.2,
        "compressed_size": 47185920,
        "uncompressed_size": 943718400,
        "num_lines": 1234567,
        "num_checkpoints": 29,
        "checkpoint_size": 33554432,
        "min_timestamp_us": 1000000,
        "max_timestamp_us": 5000000,
        "has_bloom_index": true,
        "has_checkpoint_index": true,
        "is_small": false
    }

GET /api/v1/events
++++++++++++++++++

Query events with optional filtering and pagination.

**Query Parameters:**

- ``file`` (string) - Trace file to query [required]
- ``filter`` (string) - Predicate filter (e.g., ``name=="MPI_Send"`` or ``duration>1000``)
- ``limit`` (integer) - Maximum number of events to return (default: 1000)
- ``offset`` (integer) - Offset for pagination (default: 0)
- ``include_metadata`` (boolean) - Include metadata events (default: true)

**Response:**

.. code-block:: json

    {
        "events": [
            {
                "name": "MPI_Send",
                "ph": "X",
                "ts": 1234567890,
                "dur": 12345,
                "pid": 1,
                "tid": 1
            }
        ],
        "total_events": 1234567,
        "total_matched": 42,
        "query_time_ms": 234
    }

**Example:**

.. code-block:: bash

    # Get first 100 MPI_Send events
    curl "http://localhost:8080/api/v1/events?file=trace1.pfw.gz&filter=name==\"MPI_Send\"&limit=100"

    # Get events with specific duration threshold
    curl "http://localhost:8080/api/v1/events?file=trace1.pfw.gz&filter=duration>1000&limit=50"

GET /api/v1/events/stream
+++++++++++++++++++++++++

Stream events as newline-delimited JSON (NDJSON). Useful for large result sets and real-time processing.

**Query Parameters:** Same as ``/api/v1/events``

**Response:** Stream of JSON objects, one per line

.. code-block:: text

    {"name":"MPI_Send","ph":"X","ts":1234567890,"dur":12345,"pid":1,"tid":1}
    {"name":"MPI_Recv","ph":"X","ts":1234567950,"dur":200,"pid":1,"tid":1}
    ...

**Example:**

.. code-block:: bash

    # Stream all events with duration > 5000 microseconds
    curl "http://localhost:8080/api/v1/events/stream?file=trace1.pfw.gz&filter=duration>5000"

GET /api/v1/stats
+++++++++++++++++

Retrieve aggregated statistics over events.

**Query Parameters:**

- ``file`` (string) - Trace file to analyze [required]
- ``filter`` (string) - Optional predicate filter (applied before aggregation)

**Response:**

.. code-block:: json

    {
        "total_events": 1234567,
        "total_matched": 42,
        "event_names": {
            "MPI_Send": 15,
            "MPI_Recv": 20,
            "MPI_Barrier": 7
        },
        "duration_stats": {
            "min_us": 100,
            "max_us": 45000,
            "avg_us": 1234.5,
            "median_us": 950
        }
    }

GET /api/v1/info
++++++++++++++++

Get global metadata about all trace files (time bounds, summary statistics).

**Response:**

.. code-block:: json

    {
        "total_files": 3,
        "global_min_timestamp_us": 1000000,
        "global_max_timestamp_us": 10000000,
        "total_events": 3704701,
        "file_summary": [
            {
                "path": "trace1.pfw.gz",
                "num_lines": 1234567,
                "min_timestamp_us": 1000000,
                "max_timestamp_us": 5000000
            },
            {
                "path": "trace2.pfw.gz",
                "num_lines": 1234567,
                "min_timestamp_us": 3000000,
                "max_timestamp_us": 7000000
            },
            {
                "path": "trace3.pfw.gz",
                "num_lines": 1235567,
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

- ``file`` (string) - Trace file to query [required]
- ``begin_us`` (integer) - Start time in microseconds [required]
- ``end_us`` (integer) - End time in microseconds [required]
- ``filter`` (string) - Optional predicate filter
- ``lanes`` (string) - Optional JSON-formatted lane grouping (see below)
- ``viewport_width`` (integer) - Pixel width for binning aggregation (default: 1920)
- ``include_metadata`` (boolean) - Include metadata events (default: true)

**Response:** Array of events, with shorter-duration events aggregated at higher zoom levels

.. code-block:: json

    {
        "events": [
            {
                "name": "MPI_Send",
                "ph": "X",
                "ts": 1234567890,
                "dur": 12345,
                "pid": 1,
                "tid": 1
            }
        ],
        "time_range_us": [1234567000, 1234600000],
        "viewport_width": 1920
    }

**Lane Grouping:**

Group events by process/thread lane using JSON:

.. code-block:: bash

    # Group by process ID
    curl "http://localhost:8080/api/v1/viz/events?file=trace.pfw.gz&begin_us=1000000&end_us=2000000&lanes=%7B%22fields%22:%22pid%22%7D"

    # Group by multiple criteria
    curl "http://localhost:8080/api/v1/viz/events?file=trace.pfw.gz&begin_us=1000000&end_us=2000000&lanes=%7B%22fields%22:%5B%22pid%22,%22tid%22%5D%7D"

**Example:**

.. code-block:: bash

    # Get events for visualization in time range [1M, 2M] microseconds
    curl "http://localhost:8080/api/v1/viz/events?file=trace1.pfw.gz&begin_us=1000000&end_us=2000000"

    # Same query with MPI filtering
    curl "http://localhost:8080/api/v1/viz/events?file=trace1.pfw.gz&begin_us=1000000&end_us=2000000&filter=name==\"MPI_Send\""

Event Filtering
---------------

The ``filter`` parameter supports Chrome Trace Event field predicates:

**Operators:**

- ``==`` (equality): ``name=="MPI_Send"``
- ``!=`` (inequality): ``name!="MPI_Barrier"``
- ``>`` (greater than): ``duration>1000``
- ``<`` (less than): ``duration<500``
- ``>=`` (greater than or equal): ``ts>=1000000``
- ``<=`` (less than or equal): ``ts<=2000000``

**Combining Filters:**

Multiple predicates can be combined with logical operators:

.. code-block:: bash

    # AND: Get MPI_Send events with duration > 1000
    curl "http://localhost:8080/api/v1/events?file=trace.pfw.gz&filter=name==\"MPI_Send\"&filter=duration>1000"

**Common Field Names:**

- ``name`` (string) - Event function name
- ``ph`` (string) - Phase: X (complete), B (begin), E (end), M (metadata)
- ``ts`` (integer) - Timestamp in microseconds
- ``dur`` (integer) - Duration in microseconds
- ``pid`` (integer) - Process ID
- ``tid`` (integer) - Thread ID

Indexing
--------

**Bloom Filters:**

Trace files larger than 8 MB (compressed) are automatically indexed with bloom filters (``.bidx`` files) during server startup. Bloom filters accelerate event filtering by skipping chunks that cannot contain matching events.

**Checkpoint Indexes:**

Checkpoint indexes (``.idx`` files) store byte offsets and decompression state, enabling efficient random access to events by line number or byte position.

**Small Files:**

Files smaller than 8 MB are streamed directly without sidecar indexes. The server detects this automatically based on compressed file size.

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

Event filtering streams through bloom indexes and partial reads, minimizing memory usage. Large result sets can be fetched using the ``/api/v1/events/stream`` endpoint for streaming JSON output.

**Query Optimization:**

- Use narrow time ranges in ``/api/v1/viz/events`` queries
- Apply filters to reduce the number of events scanned
- Use ``limit`` and ``offset`` for pagination
- Consider grouping by ``lanes`` for visualization queries to reduce network overhead
