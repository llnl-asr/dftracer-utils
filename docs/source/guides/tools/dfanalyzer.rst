:description: Get a trace directory into the pandas frames dfanalyzer expects using the dftracer.utils.dfanalyzer bridge over the C++ aggregation index.

Use the dfanalyzer bridge
===========================

.. admonition:: Goal
   :class: goal

   Get a trace directory into the pandas frames that
   `dfanalyzer <https://github.com/llnl-asr/dfanalyzer>`_ expects, using the
   ``dftracer.utils.dfanalyzer`` module that bridges the C++ aggregation index to
   it. This is Python-only; there is no C++ or C equivalent.

This module is dfanalyzer's integration contract - the names in its
``__all__`` co-version with dfanalyzer releases. Reach for it directly only if
you are building something dfanalyzer-shaped yourself; for general trace
analysis use :doc:`../analysis/aggregation` and :doc:`../../trace-viewer`
instead. Full signatures are in :doc:`../../api/dfanalyzer`.

The typical flow
-----------------

Point at a trace directory, build (or refresh) its index, then read frames out
of it.

.. code-block:: python

   from dftracer.utils.dfanalyzer import ensure_index, view_typed_frames, index_path_for

   trace_path = "./traces"

   # 1. Build/refresh the dftracer index for this path. Idempotent: dftracer-
   #    utils skips files whose tiers already exist, so repeat calls are cheap.
   ensure_index(
       trace_path,
       trace_groups=None,       # or a list of manifest.json group names
       time_interval_ms=1000,   # aggregation bucket width
   )

   # 2. Read the {events, profiles, system} frames dfanalyzer consumes.
   index_path = index_path_for(trace_path)
   frames = view_typed_frames([trace_path], index_path)
   events_df = frames["events"]
   profiles_df = frames["profiles"]
   system_df = frames["system"]

``ensure_index`` resolves ``trace_path`` with ``resolve_trace_inputs`` (a
directory, a glob, or a ``manifest.json`` with named trace groups), builds the
index at the conventional ``<trace_path>/.dftindex`` location
(``index_path_for``), and is inline/serial with no Dask client active.

Traces must be ``.pfw.gz``; ``resolve_trace_inputs`` only matches that suffix.

Scale the index build with Dask
---------------------------------

Pass an active ``dask.distributed`` client (or let ``ensure_index`` find one
via ``get_client()``) to fan the index build across a cluster instead of
building it inline:

.. code-block:: python

   from dask.distributed import Client
   from dftracer.utils.dfanalyzer import ensure_index

   client = Client("tcp://scheduler:8786")

   ensure_index(
       trace_path,
       trace_groups=None,
       time_interval_ms=1000,
       client=client,
       progress=lambda done, total, phase: print(f"{phase}: {done}/{total}"),
   )

Under the hood this calls ``build_index_distributed``, which is a thin
wrapper over :doc:`../scale/distributed-index`'s ``distributed_index`` plus
the dfanalyzer aggregation config (``time_interval_ms`` /
``group_by_file``). See that guide for the staging-directory layout
(``local_staging``/``shared_staging``) when you call it directly instead of
through ``ensure_index``.

Read frames through the same Dask client
-------------------------------------------

``view_typed_frames`` takes an optional ``client``; when given, the read is
delegated to ``DaskTraceViewer.collect_typed`` instead of running locally:

.. code-block:: python

   frames = view_typed_frames(
       [trace_path],
       index_path,
       time_granularity=1.0,
       time_resolution=1e6,
       duql="dur >= 1000",   # optional duql predicate
       client=client,
   )

``group_keys`` defaults to the per-file grain; pass
``typed_group_keys(file_buckets=("app_a", "app_b"))`` to fold rows onto
name substrings instead of keeping one row per file - useful when a run
touches many files and you only care about a few named groups.

High-level metrics (HLM) as a View aggregation
--------------------------------------------------

For dfanalyzer's own HLM shape (rather than the raw typed frames),
``DFAnalyzerAggregatedTraceViewer`` composes the read and the aggregation into
one View chain:

.. code-block:: python

   from dftracer.utils.dfanalyzer import DFAnalyzerAggregatedTraceViewer, HLMConfig

   viewer = DFAnalyzerAggregatedTraceViewer(
       [trace_path],
       index_path,
       hlm_config=HLMConfig(time_granularity=1.0, time_resolution=1e6),
   )
   hlm_df = viewer.hlm(["file_name", "cat"])           # from ph=X events
   profile_hlm_df = viewer.profile_hlm(["cat"])         # from ph=3 aggregates

``HLMConfig`` also carries ``ignored_file_patterns``, ``ignored_func_names``,
``ignored_func_patterns``, and ``posix_cat_rules`` - the domain rules an
analyzer preset supplies. Pass a Dask ``client`` to
``DFAnalyzerAggregatedTraceViewer`` the same way as any other
``DaskAggregatedTraceViewer``.

See also
--------

- :doc:`../../api/dfanalyzer` - full reference for every function in this
  module, including the Arrow-IPC and dtype-coercion helpers used internally
  by the distributed read path.
- :doc:`../scale/distributed-index` - the lower-level ``distributed_index``
  function this module builds on.
- :doc:`../analysis/aggregation` - the general-purpose group/agg API, if you
  are not specifically targeting dfanalyzer's frame shape.
