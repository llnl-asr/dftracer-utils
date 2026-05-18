Utilities Module
================

The ``dftracer.utils.utilities`` module provides Python bindings for
DFTracer's composable C++ utility classes. Each utility wraps a C++
pipeline stage and exposes it as a callable Python object.

Utilities fall into two categories:

- **Tabular utilities** return Arrow data via ``process()`` (materialized
  ``ArrowTable``) and ``iter_arrow()`` (streaming ``ArrowBatch``).
- **Scalar utilities** return Python dicts from ``process()``.

All utilities accept an optional ``runtime`` argument for thread pool
control. Per-call inputs (``file_path``, ``predicates``, etc.) are
passed to ``process()``, not the constructor.

All utilities are callable: ``util(...)`` is equivalent to
``util.process(...)``.

.. code-block:: python

   from dftracer.utils.utilities import (
       AggregatorUtility,
       MetadataCollectorUtility,
       ReconstructionPlannerUtility,
       ReorganizationPlannerUtility,
       StatisticsAggregatorUtility,
       StatisticsQueryUtility,
   )

Tabular Utilities (Arrow Output)
--------------------------------

These utilities return columnar Arrow data. ``process()`` returns a
materialized :class:`~dftracer.utils.arrow.ArrowTable`;
``iter_arrow()`` streams :class:`~dftracer.utils.arrow.ArrowBatch`
objects one at a time.

AggregatorUtility
~~~~~~~~~~~~~~~~~

High-level aggregation pipeline. Scans a directory for ``.pfw`` /
``.pfw.gz`` files, builds indexes, aggregates events into time-bucketed
counters, and returns the result as Arrow.

The Arrow output always includes the base aggregation columns:
``batch_type``, ``cat``, ``name``, ``pid``, ``tid``, ``hhash``,
``fhash``, ``time_bucket``, ``count``, ``dur_total``, ``dur_min``,
``dur_max``, ``dur_mean``, ``dur_std``, ``size_total``, ``size_min``,
``size_max``, ``size_mean``, ``size_std``, ``ts``, and ``te``.
When ``custom_metric_fields`` is provided, each field adds
``<field>_total``, ``<field>_min``, ``<field>_max``, ``<field>_mean``,
and ``<field>_std`` columns. ``batch_type`` distinguishes regular event,
profile-counter, and system-counter rows.

.. autoclass:: dftracer.utils.dftracer_utils_ext.AggregatorUtility(runtime: Runtime | None = None)
   :members: process, iter_arrow
   :undoc-members:

.. code-block:: python

   agg = AggregatorUtility()

   # Materialized
   table = agg.process("./traces", time_interval_ms=1000.0)

   # Include custom metrics from event args
   table = agg.process(
       "./traces",
       time_interval_ms=1000.0,
       custom_metric_fields=["bytes", "ops"],
       compute_percentiles=True,
   )

   # Streaming
   for batch in agg.iter_arrow("./traces"):
       pa_batch = pyarrow.record_batch(batch)
       process(pa_batch)

   # Callable shorthand
   table = agg("./traces")

ComparatorUtility
~~~~~~~~~~~~~~~~~

Compare trace metrics between a baseline and variant run. Returns a
hierarchical comparison with per-category and per-operation deltas,
Cohen's d significance, and regression detection.

Three output methods:

- ``compare()`` returns a materialized :class:`~dftracer.utils.arrow.ArrowTable`
  with columns: ``node_path``, ``metric_group``, ``metric_name``, ``baseline``,
  ``variant``, ``baseline_stdev``, ``variant_stdev``, ``delta``, ``pct_change``,
  ``cohens_d``, ``significance``, ``is_regression``.
- ``compare_json()`` returns a JSON string with the full hierarchical tree.
- ``compare_table()`` returns a formatted ASCII table string.

.. autoclass:: dftracer.utils.dftracer_utils_ext.ComparatorUtility(runtime: Runtime | None = None)
   :members: compare, compare_json, compare_table
   :undoc-members:

.. code-block:: python

   from dftracer.utils.utilities import ComparatorUtility

   cmp = ComparatorUtility()

   # Arrow table for programmatic analysis
   table = cmp.compare("./traces_v1/run.pfw.gz", "./traces_v2/run.pfw.gz")

   # JSON for serialization
   json_str = cmp.compare_json("./traces_v1", "./traces_v2")

   # Formatted table for display
   print(cmp.compare_table("./baseline.pfw.gz", "./variant.pfw.gz"))

   # With options
   table = cmp.compare(
       "./baseline.pfw.gz",
       "./variant.pfw.gz",
       query='cat == "POSIX"',
       time_interval_ms=1000.0,
       threshold=1.0,
   )

   # Callable shorthand (delegates to compare)
   table = cmp("./baseline.pfw.gz", "./variant.pfw.gz")

Scalar Utilities (Dict Output)
------------------------------

These utilities return Python dicts. Arrow output is not applicable
since their results are scalar or structural (not tabular).

StatisticsQueryUtility
~~~~~~~~~~~~~~~~~~~~~~

Query pre-computed statistics from an indexed trace file.
When bloom/chunk statistics are not available, the utility falls back to
streaming the file sequentially and computing statistics on-the-fly.

.. autoclass:: dftracer.utils.dftracer_utils_ext.StatisticsQueryUtility(runtime: Runtime | None = None)
   :members: process
   :undoc-members:

``process(file_path, query_type="summary", top_n=10, index_dir="")`` returns
a dict; ``query_type`` accepts ``"summary"``, ``"top_n_names"``, and other
pre-computed statistics views.

.. code-block:: python

   sq = StatisticsQueryUtility()
   result = sq.process("trace.pfw.gz", query_type="summary")
   print(result["total_events"])

   result = sq.process("trace.pfw.gz", query_type="top_n_names", top_n=5)
   for name, count in result["results"]:
       print(f"  {name}: {count}")

StatisticsAggregatorUtility
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Aggregate statistics from a trace file. Uses pre-computed chunk
statistics from the ``.dftindex`` store when available. When chunk
statistics are absent, falls back to streaming the ``.pfw.gz``
line-by-line and computing statistics on-the-fly.

.. autoclass:: dftracer.utils.dftracer_utils_ext.StatisticsAggregatorUtility(runtime: Runtime | None = None)
   :members: process
   :undoc-members:

.. code-block:: python

   sa = StatisticsAggregatorUtility()
   result = sa.process("trace.pfw.gz")
   print(f"Events: {result['total_events']}")
   print(f"Duration mean: {result['duration_mean_us']} us")

MetadataCollectorUtility
~~~~~~~~~~~~~~~~~~~~~~~~

Collect metadata from a DFTracer trace file.

.. autoclass:: dftracer.utils.dftracer_utils_ext.MetadataCollectorUtility(runtime: Runtime | None = None)
   :members: process
   :undoc-members:

.. code-block:: python

   mc = MetadataCollectorUtility()
   result = mc.process("trace.pfw.gz")
   print(f"Size: {result['size_mb']:.2f} MB")
   print(f"Format: {result['format']}")
   print(f"Events: {result['valid_events']}")

ReorganizationPlannerUtility
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Plan semantic reorganization of trace files. When manifest data is
available in the ``.dftindex`` store, produces per-checkpoint extraction
tasks. When manifest tables are absent, falls back to streaming the file
line-by-line and emitting one whole-file extraction task per query group.

.. autoclass:: dftracer.utils.dftracer_utils_ext.ReorganizationPlannerUtility(runtime: Runtime | None = None)
   :members: process
   :undoc-members:

.. code-block:: python

   rp = ReorganizationPlannerUtility()
   plan = rp.process(
       source_files=["trace1.pfw.gz", "trace2.pfw.gz"],
       groups=[{"name": "posix", "query": 'cat == "POSIX"'}],
   )
   print(f"Tasks: {len(plan['tasks'])}")

ReconstructionPlannerUtility
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Plan reconstruction of original files from reorganized traces.

.. autoclass:: dftracer.utils.dftracer_utils_ext.ReconstructionPlannerUtility(runtime: Runtime | None = None)
   :members: process
   :undoc-members:

.. code-block:: python

   rcp = ReconstructionPlannerUtility()
   plan = rcp.process(reorganized_files=["reorg1.pfw.gz"])
   print(f"Segments: {plan['total_segments']}")

Arrow Data Types
----------------

.. autoclass:: dftracer.utils.arrow.ArrowBatch
   :members:
   :undoc-members:

.. autoclass:: dftracer.utils.arrow.ArrowTable
   :members:
   :undoc-members:
