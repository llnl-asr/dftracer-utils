Python API Reference
====================

This section contains the Python API documentation for dftracer utilities.

.. toctree::
   :maxdepth: 2
   :caption: Python Modules:

   trace_reader
   utilities
   runtime
   reader
   indexer
   dfanalyzer
   query

Module Overview
---------------

The dftracer utilities Python package provides the following main modules:

- :doc:`trace_reader` - Streaming trace file reader with JSON and Arrow support
- :doc:`utilities` - Utility bindings (statistics, views, aggregation, bloom queries)
- :doc:`runtime` - Coroutine runtime and Dask integration
- :doc:`reader` - Lazy JSON object type
- :doc:`indexer` - Indexing and searching capabilities
- :doc:`dfanalyzer` - dfanalyzer bridge: index build, Arrow IPC, distributed HLM
