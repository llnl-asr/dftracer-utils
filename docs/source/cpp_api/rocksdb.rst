RocksDB
=======

The RocksDB layer provides the shared storage backend used by the
root-local ``.dftindex`` and provenance stores introduced by the
RocksDB migration.

It includes:

- database wrappers and lifecycle management
- column-family and merge-operator registration for the ``.dftindex`` schema
- key encoding helpers for typed prefix/range scans
- manager utilities for sharing open database handles across readers,
  indexers, and higher-level composites
- bulk-ingest helpers (``SstFileWriter`` + ``IngestExternalFile``) used by
  the distributed indexing pipeline

Architecture
------------

.. mermaid::

   graph TD
       Readers["TraceReader / utilities"] --> Manager["RocksDBManager"]
       Indexers["Indexer / provenance writers"] --> Manager
       Manager --> Database["RocksDatabase"]
       Database --> CFs["Column families"]
       Database --> Codec["KeyCodec"]
       Database --> Merge["MergeOperators (AGGREGATION, SYSTEM_METRICS)"]
       CFs --> Store[".dftindex / provenance store"]
       Codec --> Store

See also:

- :doc:`api/rocksdb`
- :doc:`indexer`
