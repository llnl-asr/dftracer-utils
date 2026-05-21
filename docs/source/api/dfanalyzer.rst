DFAnalyzer Module
=================

The ``dftracer.utils.dfanalyzer`` module bridges the C++ aggregation index to
`dfanalyzer <https://github.com/hariharan-devarajan/dfanalyzer>`_. It provides
the index-build, Arrow-IPC marshalling, and distributed high-level-metrics
(HLM) helpers that dfanalyzer drives over a Dask cluster.

These helpers only depend on the :class:`~dftracer.utils.Indexer` and the Arrow
plumbing, so they live in ``dftracer-utils`` rather than being vendored inside
dfanalyzer.

Dask is an optional dependency -- the distributed helpers require
``dask.distributed`` to be installed.

Index Building
--------------

.. autofunction:: dftracer.utils.dfanalyzer.resolve_trace_inputs

.. autofunction:: dftracer.utils.dfanalyzer.index_path_for

.. autofunction:: dftracer.utils.dfanalyzer.build_index_distributed

.. autofunction:: dftracer.utils.dfanalyzer.ensure_index

Arrow IPC Marshalling
---------------------

The C extension yields Arrow data as PyCapsules. These helpers convert between
capsules, Arrow IPC byte streams (the wire format moved between Dask workers),
and pandas frames.

.. autofunction:: dftracer.utils.dfanalyzer.batches_to_ipc

.. autofunction:: dftracer.utils.dfanalyzer.ipc_to_pandas

.. autofunction:: dftracer.utils.dfanalyzer.scan_to_ipc

Distributed High-Level Metrics
------------------------------

The HLM pipeline aggregates the per-worker aggregation column family into a
Dask DataFrame. Each worker owns a disjoint PID set, so per-worker partials
have disjoint keys and need no cross-worker merge.

.. autofunction:: dftracer.utils.dfanalyzer.distributed_hlm

.. autofunction:: dftracer.utils.dfanalyzer.worker_hlm_partial

.. autofunction:: dftracer.utils.dfanalyzer.make_empty_hlm

View Groupby Partials
---------------------

These helpers implement mergeable per-partition view aggregation: each
partition emits partial aggregates (sum, count, min, max, sum-of-squares) that
are combined and finalized into mean/std without a global shuffle.

.. autofunction:: dftracer.utils.dfanalyzer.partial_arrow_view_groupby

.. autofunction:: dftracer.utils.dfanalyzer.finalize_view_partials

.. autofunction:: dftracer.utils.dfanalyzer.build_partial_meta

.. autofunction:: dftracer.utils.dfanalyzer.build_final_meta

Dtype Coercion
--------------

Utilities that normalize Arrow-backed dtypes into the pandas-native dtypes
expected by dfanalyzer's downstream ``metrics.py``.

.. autofunction:: dftracer.utils.dfanalyzer.normalize_arrow_dtypes

.. autofunction:: dftracer.utils.dfanalyzer.coerce_arrow_numerics_to_pandas_native

.. autofunction:: dftracer.utils.dfanalyzer.coerce_profile_dtypes
