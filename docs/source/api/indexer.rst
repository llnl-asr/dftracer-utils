Indexer Module
==============

The indexer module provides functionality for indexing DFTracer trace files
(``.pfw`` / ``.pfw.gz``) backed by a ``.dftindex`` RocksDB store. The
top-level :class:`~dftracer.utils.Indexer` follows a ``resolve`` / ``build``
pattern over a directory or file list and exposes the higher index tiers
(checkpoints, bloom filters, manifests, aggregation).
:class:`~dftracer.utils.CheckpointIndexer` is the lower-level single-file
interface used for checkpoint-level operations.

Indexer Class
-------------

.. autoclass:: dftracer.utils.Indexer(directory: str = '', files: list[str] | None = None, index_dir: str = '', require_checkpoint: bool = True, require_bloom: bool = True, require_manifest: bool = True, require_aggregation: bool | AggregationConfig | None = None, checkpoint_size: int = 33554432, parallelism: int = 0, force_rebuild: bool = False, runtime: Runtime | None = None)
   :members: resolve, build, ensure_indexed, get_checkpoint_indexer, get_hash_table, query_file_pids, query_all_file_pids, query_file_info, iter_aggregation, iter_arrow_dfanalyzer, iter_arrow_dfanalyzer_all
   :undoc-members:
   :show-inheritance:

Aggregation is enabled by passing ``require_aggregation=True`` (defaults) or
``require_aggregation=AggregationConfig(...)``. The aggregation knobs
(``time_interval_ms``, ``group_keys``, ``custom_metric_fields``,
``compute_percentiles``) are fields of :class:`~dftracer.utils.AggregationConfig`,
not direct ``Indexer`` arguments.

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

.. autoclass:: dftracer.utils.CheckpointIndexer(gz_path: str, index_path: str | None = None, checkpoint_size: int = 1048576, force_rebuild: bool = False, build_bloom: bool = False, build_manifest: bool = False, runtime: Runtime | None = None)
   :members:
   :undoc-members:
   :show-inheritance:
   :special-members: __enter__, __exit__

IndexerCheckpoint Class
-----------------------

.. autoclass:: dftracer.utils.IndexerCheckpoint
   :members:
   :undoc-members:
   :show-inheritance:

Distributed Index (SST-based)
-----------------------------

The distributed-index path lets the coordinator pre-register files, hand out
``file_id`` ranges to workers, and bulk-ingest worker-produced SST artifacts
back into the unified ``.dftindex`` store.

IndexDatabase
~~~~~~~~~~~~~

.. autoclass:: dftracer.utils.dftracer_utils_ext.IndexDatabase(index_path: str)
   :members: init_schema, register_files, reserve_file_id_range, bulk_ingest, rebuild_root_summaries, write_agg_global_config, write_agg_file_markers, write_aggregation_tracker
   :undoc-members:

SstArtifactRegistry
~~~~~~~~~~~~~~~~~~~

.. autoclass:: dftracer.utils.dftracer_utils_ext.SstArtifactRegistry
   :members: append
   :undoc-members:

Module-level Functions
----------------------

.. autofunction:: dftracer.utils.dftracer_utils_ext.scan_files

.. autofunction:: dftracer.utils.dftracer_utils_ext.scan_aggregation_manifest

.. autofunction:: dftracer.utils.dftracer_utils_ext.build_sst_batch

.. autofunction:: dftracer.utils.dftracer_utils_ext.plan_lpt_partition

.. autofunction:: dftracer.utils.dftracer_utils_ext.enumerate_gzip_members

.. autofunction:: dftracer.utils.dftracer_utils_ext.plan_work_units

.. autofunction:: dftracer.utils.dftracer_utils_ext.move_artifacts

.. autofunction:: dftracer.utils.dftracer_utils_ext.enable_aggregation_deterministic_ids

Dask Helpers
------------

The ``dftracer.utils.dask`` module provides Dask-distributed drivers built on
the SST-based primitives above:

.. autofunction:: dftracer.utils.dask.distributed_index

.. autofunction:: dftracer.utils.dask.distributed_aggregate

Dask is an optional dependency -- this module is only importable when
``dask.distributed`` is installed.
