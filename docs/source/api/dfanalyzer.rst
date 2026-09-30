:description: Reference for the dfanalyzer bridge: build high-level metrics as a View aggregation and drive index builds over a Dask cluster.

DFAnalyzer Module
=================

The ``dftracer.utils.dfanalyzer`` module bridges the C++ aggregation index to
`dfanalyzer <https://github.com/llnl-asr/dfanalyzer>`_. It builds the
high-level metrics (HLM) as a :class:`~dftracer.utils.TraceViewer` aggregation,
plus the index-build, typed-read, and dtype-coercion helpers dfanalyzer drives
over a Dask cluster.

Dask is an optional dependency -- the distributed helpers require
``dask.distributed``.

View-based HLM
--------------

``DFAnalyzerAggregatedTraceViewer`` is a
:class:`~dftracer.utils.dask.DaskAggregatedTraceViewer` subclass that holds the
dfanalyzer HLM domain rules (ignored funcs/files, POSIX category suffixes) and
composes the events and profile HLM as a single View aggregation. ``HLMConfig``
carries the rule set.

Type relationships
------------------

How the dfanalyzer bridge types relate:

.. mermaid:: /_generated/py_dfanalyzer.mmd

.. autoclass:: dftracer.utils.dfanalyzer.DFAnalyzerAggregatedTraceViewer
   :members: hlm, profile_hlm

.. autoclass:: dftracer.utils.dfanalyzer.HLMConfig

Index Building
--------------

.. autofunction:: dftracer.utils.dfanalyzer.resolve_trace_inputs

.. autofunction:: dftracer.utils.dfanalyzer.index_path_for

.. autofunction:: dftracer.utils.dfanalyzer.count_index_files

.. autofunction:: dftracer.utils.dfanalyzer.build_index_distributed

.. autofunction:: dftracer.utils.dfanalyzer.ensure_index

Typed Reads
-----------

The typed read maps one ``collect_typed`` pass over the aggregation index to the
``{events, profiles, system}`` frames dfanalyzer consumes.

.. autofunction:: dftracer.utils.dfanalyzer.typed_group_keys

.. autofunction:: dftracer.utils.dfanalyzer.view_typed_frames

.. autofunction:: dftracer.utils.dfanalyzer.build_read_frames

View Groupby Partials
---------------------

Mergeable per-partition view aggregation: each partition emits partial
aggregates (sum, count, min, max, and the centered moments ``m2`` and the mean
as a ``mean_hi`` + ``mean_lo`` pair). ``merge_view_partials`` combines the
partials of one view row (the moments with the pairwise formula, the sum, min, max and set
columns by their own rule) and ``finalize_view_partials``
turns them into mean and std without a global shuffle. The variance is never
rebuilt from a sum of squares, which loses every digit when the mean is large
next to the spread (durations near 1e9 with a spread of 1 gave a relative error
of 1.0).

.. autofunction:: dftracer.utils.dfanalyzer.partial_arrow_view_groupby

.. autofunction:: dftracer.utils.dfanalyzer.merge_view_partials

.. autofunction:: dftracer.utils.dfanalyzer.finalize_view_partials

.. autofunction:: dftracer.utils.dfanalyzer.build_partial_meta

.. autofunction:: dftracer.utils.dfanalyzer.build_final_meta

Dtype Coercion
--------------

Normalize Arrow-backed dtypes into the pandas-native dtypes expected by
dfanalyzer's downstream ``metrics.py``.

.. autofunction:: dftracer.utils.dfanalyzer.normalize_arrow_dtypes

.. autofunction:: dftracer.utils.dfanalyzer.coerce_arrow_numerics_to_pandas_native
