C++ API Reference
=================

This section contains the C++ API documentation for dftracer utilities.

.. note::
   The C++ API documentation is generated using Doxygen and Breathe.
   Make sure to run Doxygen before building the documentation.

.. toctree::
    :maxdepth: 2
    :caption: C++ Components:

    reader
    indexer
    pipeline
    coro
    task_graph
    utilities
    io
    sqlite
    scheduler
    core_common
    dft_aggregators
    dft_indexing

Overview
--------

The dftracer utilities C++ library is organized into several namespaces:

- ``dftracer::utils::core`` - Core utilities and data structures
- ``dftracer::utils::reader`` - Trace file reading
- ``dftracer::utils::indexer`` - Indexing capabilities
- ``dftracer::utils::pipeline`` - Processing pipelines
- ``dftracer::utils::coro`` - C++20 coroutine primitives
- ``dftracer::utils::task_graph`` - DAG-based task graph builder
- ``dftracer::utils::utilities`` - Composable processing utilities
- ``dftracer::utils::io`` - Async I/O backends (io_uring, kqueue, thread pool)
- ``dftracer::utils::sqlite`` - Async SQLite database operations
- ``dftracer::utils::utilities::composites::dft::aggregators`` - Event aggregation pipeline
- ``dftracer::utils::utilities::composites::dft::indexing`` - Bloom filter indexing system

.. mermaid::

   graph TB
       subgraph Core["dftracer::utils (Core)"]
           Pipeline["Pipeline"]
           Executor["Executor"]
           Scheduler["Scheduler"]
           Watchdog["Watchdog"]
           TimerService["TimerService"]
       end

       subgraph Coro["dftracer::utils::coro"]
           CoroTask["CoroTask&lt;T&gt;"]
           Channel["Channel&lt;T&gt;"]
           Generator["Generator&lt;T&gt;"]
           CoroScope["CoroScope"]
       end

       subgraph IO["dftracer::utils::io"]
           IoBackend["IoBackend"]
           IoAwaitable["IoAwaitable"]
       end

       subgraph SQLite["dftracer::utils::sqlite"]
           SqliteDB["SqliteDatabase"]
           SqliteAwait["SqliteAwaitable"]
       end

       subgraph Utilities["dftracer::utils::utilities"]
           Reader["Reader"]
           Indexer["Indexer"]
           Compression["Compression"]
           Hash["Hash"]
           FileIO["FileIO"]
           Text["Text"]
           Filesystem["Filesystem"]
           Statistics["Statistics"]
       end

       subgraph DFT["dftracer::utils::composites::dft"]
           Aggregators["Aggregators"]
           Indexing["Indexing"]
           Views["Views"]
           CallTree["Call Tree"]
       end

       subgraph TaskGraph["dftracer::utils::task_graph"]
           TG["TaskGraph"]
           TGrp["TaskGroup"]
       end

       TG --> Pipeline
       Pipeline --> Executor
       Pipeline --> Scheduler
       Executor --> IoBackend
       Executor --> SqliteDB
       Executor --> CoroTask
       Watchdog --> Executor
       TimerService --> Executor
       DFT --> Utilities
       Aggregators --> Channel
       Indexing --> Channel

Main Classes
------------

See the individual component pages above for detailed API documentation.
