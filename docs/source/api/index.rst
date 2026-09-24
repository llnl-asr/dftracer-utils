:description: Index of the dftracer.utils Python API: TraceViewer, DataFrame, Series, the query and columnar DSLs, runtime, indexer, and JIT.

Python API Reference
====================

This section contains the Python API documentation for dftracer utilities.

.. toctree::
   :maxdepth: 2
   :caption: Python Modules:

   trace_viewer
   query
   columnar
   dataframe
   series
   enums
   runtime
   reader
   indexer
   schemas
   dfanalyzer
   jit

Module Overview
---------------

The dftracer utilities Python package provides the following main modules:

- :doc:`trace_viewer` - **TraceViewer**, the primary lazy, Arrow-first query API (filter, group_by, agg, collect)
- :doc:`query` - The filter DSL used by TraceViewer
- :doc:`columnar` - The value-expression DSL (``col``/``F``/``lit``, ``Agg``, ``GroupBy``, ``eval_many``) over the DataFrame engine
- :doc:`dataframe` - **DataFrame**, the columnar frame type (the relational and
  reshape primitives - joins, window functions, gap fill, unnest - are native
  methods here)
- :doc:`series` - **Series**, the columnar column type
- :doc:`enums` - typed vocabularies (``Phase``, ``GroupKey``, ``AggOp``) and the
  ``TimeUnit`` scaling target for the builder API
- :doc:`runtime` - Coroutine runtime and Dask integration
- :doc:`reader` - Lazy JSON object type
- :doc:`indexer` - Indexing and searching capabilities
- :doc:`schemas` - Record schemas: declare, register, detect and explain record formats
- :doc:`dfanalyzer` - dfanalyzer bridge: View-based HLM, Arrow IPC, index build
