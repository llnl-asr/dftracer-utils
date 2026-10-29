:description: Reference for the Python Indexer and CheckpointIndexer: resolve and build the .dftindex store and query checkpoints, bloom filters, and aggregation.

Indexer Module
==============

The indexer module provides functionality for indexing DFTracer trace files
(``.pfw.gz``) backed by a ``.dftindex`` store. The
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

.. autoclass:: dftracer.utils.Indexer
   :members: resolve, build, ensure_indexed, get_checkpoint_indexer, rowset, query_file_pids, query_all_file_pids, query_file_info
   :undoc-members:
   :show-inheritance:

Every index build makes the aggregation tier, with one standard config: 1 s
windows, the percentile sketches on, and the file hash in the key. The
aggregation knobs (``time_interval_ms``, ``group_keys``,
``custom_metric_fields``, ``compute_percentiles``, ``group_by_file``) are
fields of :class:`~dftracer.utils.AggregationConfig`, not direct ``Indexer``
arguments, and ``aggregation_config=AggregationConfig(...)`` customizes the
tier. There is no switch that turns it off.

Three arguments steer what the build keeps. ``extensions`` picks the pruning
extensions from ``"zonemap"``, ``"bloom"``, ``"counts"`` and ``"postings"``
(all four by default). ``schema`` names the record schema of every trace, as a
registered id or a ``RecordSchema`` class, and ``None`` detects it per file.
``memory_budget`` takes bytes or a size such as ``"4GB"`` and bounds what the
build holds at once. ``require_bloom`` takes ``True`` or a
:class:`~dftracer.utils.BloomConfig`, and ``checkpoint_size`` takes bytes or a
size string.

AggregationConfig
-----------------

Configuration for the aggregation tier, passed via
``aggregation_config=AggregationConfig(...)``. Its defaults are the standard
config. Exported from ``dftracer.utils``.

.. autoclass:: dftracer.utils.AggregationConfig
   :members:
   :no-index:
   :undoc-members:

BloomConfig
-----------

Configuration for the bloom tier, passed via ``require_bloom=BloomConfig(...)``.
Exported from ``dftracer.utils``.

.. autoclass:: dftracer.utils.BloomConfig
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

Distributed Index
-----------------

The distributed-index path lets a coordinator fan one indexing task per Dask
worker, and each worker writes its files straight into the unified
``.dftindex`` store. The public entry point is
:func:`~dftracer.utils.dask.distributed_index`; it assigns the file-to-worker
split on the coordinator and lets every worker number and write its own files.
The scan and LPT-partition primitives it drives are internal to the
native extension and have no public Python path.

.. autofunction:: dftracer.utils.dask.distributed_index

Dask is an optional dependency. This module is only importable when
``dask.distributed`` is installed.
