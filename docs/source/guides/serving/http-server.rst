:description: Run dftracer_server over a trace directory and query the index from any HTTP client over its JSON REST API, and serve the web UI.

Serve and query traces over HTTP
================================

.. admonition:: Goal
   :class: goal

   Run the ``dftracer_server`` over a directory of ``.pfw.gz`` traces and
   query the index from any HTTP client. The server loads or builds sidecar
   indexes on startup, then answers JSON requests over its REST API and serves the
   :doc:`../../trace-viewer` web UI.

This guide is CLI-primary: the server is a compiled binary with no Python
binding. Any HTTP client (``curl``, a browser, an editor webview) can call the
routes.

Start the server
----------------

.. code-block:: bash

   # Serve ./traces on the default 127.0.0.1:8080
   dftracer_server -d ./traces

   # Expose on all interfaces (see Access token below), custom port
   dftracer_server -d ./traces -b 0.0.0.0 -p 9000

   # Keep indexes on fast local disk when traces live on NFS
   dftracer_server -d ./traces --index-dir /var/cache/dftracer

The server scans ``-d`` on startup, loads or builds the ``.dftindex`` stores,
and listens for requests. It also serves the interactive trace viewer at ``/``
and ``/index.html``.

Server flags
~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 30 25 45

   * - Flag
     - Default
     - Meaning
   * - ``-d``, ``--directory``
     - (required)
     - Trace directory to serve. Must exist.
   * - ``-b``, ``--bind``
     - ``127.0.0.1``
     - Bind address. Use ``0.0.0.0`` to reach it from other hosts.
   * - ``-p``, ``--port``
     - ``8080``
     - Listen port.
   * - ``--index-dir``
     - (falls back to ``-d``)
     - Directory for the root-local ``.dftindex`` stores.
   * - ``--token``
     - ``""``
     - Access token. When set, every request must present it.
   * - ``--checkpoint-size``
     - build default
     - Decompression checkpoint interval (bytes) for auto-indexing. Smaller
       gives finer zoom-in seeks at the cost of a larger index.
   * - ``--member-cache-size``
     - ``1073741824`` (1 GB)
     - Bytes of decoded gzip members retained to share across concurrent
       queries. ``0`` disables retention.
   * - ``--executor-threads``
     - hardware concurrency
     - Worker threads for concurrent request handling.

Access token
~~~~~~~~~~~~

The server binds to loopback by default, so it is not reachable off-host unless
you pass ``-b 0.0.0.0``. When you expose it, set a token:

.. code-block:: bash

   dftracer_server -d ./traces -b 0.0.0.0 --token "$(openssl rand -hex 16)"

With ``--token`` set, every request must present the token, either as a
``?token=<TOKEN>`` query parameter or an ``Authorization: Bearer <TOKEN>``
header; otherwise the server responds ``401 Unauthorized``.

Query the API
-------------

Every route returns a single JSON response. Start with the global summary and
the file list:

.. code-block:: bash

   curl 'http://127.0.0.1:8080/api/info'
   # {"file_count":2,"global_min_timestamp_us":1000000,"global_max_timestamp_us":6999732}

   curl 'http://127.0.0.1:8080/api/files'
   # {"files":[{"path":"trace-0.pfw.gz","has_bloom_data":true}],"count":1}

Fetch metadata for one file, and resolve content hashes back to names:

.. code-block:: bash

   curl 'http://127.0.0.1:8080/api/files/info?file=trace-0.pfw.gz'

   # type defaults to "file"; also host, string, proc. Comma-separate hashes.
   curl 'http://127.0.0.1:8080/api/resolve?type=file&hash=314c1a1cdb22a136'
   # {"names":{"314c1a1cdb22a136":"/data/train/img_0.npz"}}

Data routes
~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 12 30 58

   * - Method
     - Path
     - Description
   * - GET
     - ``/api/info``
     - File count and global time bounds. No params.
   * - GET
     - ``/api/files``
     - List the indexed trace files. No params.
   * - GET
     - ``/api/files/info``
     - Metadata for one file. Param: ``file`` (required).
   * - GET
     - ``/api/resolve``
     - Resolve hashes to names. Params: ``hash`` (required, comma-separated),
       ``type`` (default ``file``; ``file``/``host``/``string``/``proc``).
   * - POST
     - ``/api/cancel``
     - Cancel an in-flight request. Param: ``id`` (the request's
       ``X-Request-Id``). Returns ``{"cancelled":true|false}``.

Visualization routes
~~~~~~~~~~~~~~~~~~~~~

The ``/api/viz/*`` routes back the trace viewer. Most take a shared time window
- ``begin`` and ``end`` (required; pass ``0``/``999999999`` for the whole
trace) - and a ``summary`` level-of-detail knob (``1`` = full detail).
``/api/viz/breaks`` and ``/api/viz/proctree`` need no time window; they look at
the whole trace.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Path
     - Description and extra params
   * - ``/api/viz/events``
     - Time-windowed, LOD-aggregated events for rendering. Extra: ``pid``,
       ``cat``, ``query`` (a DSL predicate, e.g. ``dur >= 1000``).
   * - ``/api/viz/density``
     - Sub-pixel events bucketed into density blocks. Extra: ``width``
       (default ``1920``); ``summary`` defaults to ``2`` here.
   * - ``/api/viz/breaks``
     - Globally-idle time gaps and multi-run detection. Param:
       ``ts_normalize`` (default ``1``).
   * - ``/api/viz/counters``
     - Read/write bytes and I/O op counts per time bucket.
   * - ``/api/viz/proctree``
     - Inferred process/fork hierarchy with host, rank, I/O. Param: ``file``.
   * - ``/api/viz/columns``
     - Groupable columns present in the trace. No params.
   * - ``/api/viz/stats``
     - Per-name aggregation over the time range.
   * - ``/api/viz/calltree``
     - Merged flamegraph tree from ts/dur containment. Extra: ``group`` (set
       to ``pid`` to keep processes separate).
   * - ``/api/viz/histogram``
     - Duration distribution: percentiles and log-spaced buckets. Extra:
       ``query``.
   * - ``/api/viz/layers``
     - Operation-name to category map. No params.

A machine-readable OpenAPI 3.1 spec is served at ``/api/openapi.json``, and an
interactive explorer at ``/api``.

Cancel a slow request
---------------------

Send an ``X-Request-Id`` header with a long-running query, then POST to
``/api/cancel`` with that id to abort it:

.. code-block:: bash

   curl -H 'X-Request-Id: q42' \
     'http://127.0.0.1:8080/api/viz/calltree?begin=0&end=999999999&summary=1' &

   curl -X POST 'http://127.0.0.1:8080/api/cancel?id=q42'
   # {"cancelled":true}

See also
--------

- :doc:`../../trace-viewer` - the web UI the server hosts.
- :doc:`../core/query-dsl` - the predicate language accepted by the ``query``
  viz params.
- :doc:`../../cli` - ``dftracer_view --call-tree`` / ``--flamegraph`` build
  containment trees offline instead of over HTTP.
