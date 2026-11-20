:description: Index of the dftracer.utils Python API: TraceViewer, DataFrame, Series, the duql and columnar builders, runtime, indexer, and JIT.

Python API Reference
====================

This section contains the Python API documentation for dftracer utilities.

.. toctree::
   :maxdepth: 2
   :caption: Python Modules:

   trace_viewer
   trace_writer
   table
   duql
   columnar
   dataframe
   lazyframe
   series
   enums
   runtime
   errors
   indexer
   store
   schemas
   jit
   plugins

Module Overview
---------------

The dftracer utilities Python package provides the following main modules:

- :doc:`trace_viewer` - **TraceViewer**, the primary lazy, Arrow-first query API (filter, group_by, agg, collect)
- :doc:`trace_writer` - **TraceWriter**, which writes DFTracer records to a gzip or plain trace file
- :doc:`table` - **write_table**, **read_table** and **TableWriter**, which store a pandas frame as a typed gzip JSON-lines file
- :doc:`duql` - The duql filter builder used by TraceViewer
- :doc:`columnar` - The value-expression DSL (``col``/``F``/``lit``, ``Agg``, ``GroupBy``, ``eval_many``) over the DataFrame engine
- :doc:`dataframe` - **DataFrame**, the columnar frame type (the relational and
  reshape primitives - joins, window functions, gap fill, unnest - are native
  methods here)
- :doc:`lazyframe` - **LazyFrame**, **lazy** and **scan_file**, the deferred query plan
- :doc:`series` - **Series**, the columnar column type
- :doc:`enums` - typed vocabularies (``GroupKey``, ``AggOp``) and the
  ``TimeUnit`` scaling target (``Phase`` lives in ``dftracer.utils.trace``)
- :doc:`runtime` - Coroutine runtime and Dask integration
- :doc:`errors` - The ``DFTUtilsError`` hierarchy every call raises
- :doc:`plugins` - ``load_plugin`` and ``PluginHandle``, which load a compiled plugin for a block of code
- :doc:`indexer` - Indexing and searching capabilities
- :doc:`store` - **Store**, the key-value store
- :doc:`schemas` - Record schemas: declare, register, detect and explain record formats
