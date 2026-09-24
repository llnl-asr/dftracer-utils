:description: Reference for the Python Indexer and CheckpointIndexer: resolve and build the .dftindex store and query checkpoints, bloom filters, and aggregation.

Indexer Module
==============

The indexer module provides functionality for indexing DFTracer trace files
(``.pfw.gz``) backed by a ``.dftindex`` RocksDB store. The
top-level :class:`~dftracer.utils.Indexer` follows a ``resolve`` / ``build``
pattern over a directory or file list and exposes the higher index tiers
(checkpoints, bloom filters, aggregation).
:class:`~dftracer.utils.CheckpointIndexer` is the lower-level single-file
interface used for checkpoint-level operations.

Indexer Class
-------------

Type relationships
------------------

How the indexer types relate:

.. mermaid:: /_generated/py_indexer.mmd

.. autoclass:: dftracer.utils.Indexer(directory: str = '', files: list[str] | None = None, index_dir: str = '', require_checkpoint: bool = True, require_bloom: bool = True, build_bloom: bool = True, require_aggregation: bool | AggregationConfig | None = None, checkpoint_size: int = 33554432, parallelism: int = 0, force_rebuild: bool = False, runtime: Runtime | None = None)
   :members: resolve, build, ensure_indexed, get_checkpoint_indexer, get_dictionary, query_file_pids, query_all_file_pids, query_file_info
   :undoc-members:
   :show-inheritance:

Aggregation is enabled by passing ``require_aggregation=True`` (defaults) or
``require_aggregation=AggregationConfig(...)``. The aggregation knobs
(``time_interval_ms``, ``group_keys``, ``custom_metric_fields``,
``compute_percentiles``, ``group_by_file``) are fields of
:class:`~dftracer.utils.AggregationConfig`, not direct ``Indexer`` arguments.

AggregationConfig
-----------------

Configuration for the aggregation tier, passed via
``require_aggregation=AggregationConfig(...)``. Exported from
``dftracer.utils``.

.. autoclass:: dftracer.utils.AggregationConfig
   :members:
   :no-index:
   :undoc-members:

IndexStatus
-----------

Result of :meth:`~dftracer.utils.Indexer.resolve` /
:meth:`~dftracer.utils.Indexer.ensure_indexed`. Exported from
``dftracer.utils``.

.. autoclass:: dftracer.utils.IndexStatus
   :members:
   :no-index:
   :undoc-members:

CheckpointIndexer Class
-----------------------

.. autoclass:: dftracer.utils.CheckpointIndexer(gz_path: str, index_path: str | None = None, checkpoint_size: int = 1048576, force_rebuild: bool = False, build_bloom: bool = False, runtime: Runtime | None = None)
   :members:
   :undoc-members:
   :show-inheritance:
   :special-members: __enter__, __exit__

Distributed Index (SST-based)
-----------------------------

The distributed-index path lets a coordinator pre-register files, hand out
``file_id`` ranges to Dask workers, and bulk-ingest worker-produced SST
artifacts back into the unified ``.dftindex`` store. The public entry point is
:func:`~dftracer.utils.dask.distributed_index`; it pre-registers files, fans
one indexing task per worker, and ingests the resulting SSTs. The scan,
LPT-partition, and SST-registry primitives it drives are internal to the
native extension and have no public Python path.

.. autofunction:: dftracer.utils.dask.distributed_index

Dask is an optional dependency -- this module is only importable when
``dask.distributed`` is installed.
