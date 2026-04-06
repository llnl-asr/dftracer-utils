RocksDB
=======

The RocksDB layer provides the shared storage backend used by the
root-local ``.dftindex`` and provenance stores introduced by the
RocksDB migration.

It includes:

- database wrappers and lifecycle management
- async awaitables for database work on executor-backed threads
- key encoding helpers for typed prefix/range scans
- manager utilities for sharing open database handles across readers,
  indexers, and higher-level composites

Architecture
------------

.. mermaid::

   graph TD
       Readers["TraceReader / utilities"] --> Manager["RocksDBManager"]
       Indexers["Indexer / provenance writers"] --> Manager
       Manager --> Database["RocksDatabase"]
       Database --> CFs["Column families"]
       Database --> Async["DbAwaitable / rocks::run"]
       Database --> Codec["KeyCodec"]
       CFs --> Store[".dftindex / provenance store"]
       Async --> Runtime["Executor-backed threads"]
       Codec --> Store

See also:

- :doc:`api/rocksdb`
- :doc:`indexer`
