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
       BloomQueryUtility,
       MetadataCollectorUtility,
       ReconstructionPlannerUtility,
       ReorganizationPlannerUtility,
       StatisticsAggregatorUtility,
       StatisticsQueryUtility,
       ViewBuilderUtility,
       ViewReaderUtility,
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

.. autoclass:: dftracer.utils.dftracer_utils_ext.AggregatorUtility(runtime: Runtime | None = None)
   :members: process, iter_arrow
   :undoc-members:

.. code-block:: python

   agg = AggregatorUtility()

   # Materialized
   table = agg.process("./traces", time_interval=1.0, categories=["POSIX"])
   # table is an ArrowTable with 18 columns:
   # cat, name, pid, tid, hhash, fhash, time_bucket, count,
   # dur_total, dur_min, dur_max, dur_mean,
   # size_total, size_min, size_max, size_mean, ts, te

   # Streaming
   for batch in agg.iter_arrow("./traces"):
       pa_batch = pyarrow.record_batch(batch)
       process(pa_batch)

   # Callable shorthand
   table = agg("./traces")

ViewReaderUtility
~~~~~~~~~~~~~~~~~

Read events from a trace file filtered by bloom-filter predicates.
Returns matched events as Arrow columns (dynamic schema derived from
JSON keys).

.. autoclass:: dftracer.utils.dftracer_utils_ext.ViewReaderUtility(runtime: Runtime | None = None)
   :members: process, iter_arrow
   :undoc-members:

.. code-block:: python

   vr = ViewReaderUtility()

   # Materialized
   table = vr.process("trace.pfw.gz", predicates={"cat": ["POSIX"]})

   # Streaming
   for batch in vr.iter_arrow("trace.pfw.gz", predicates={"cat": ["POSIX"]}):
       df = polars.from_arrow(batch)

Scalar Utilities (Dict Output)
------------------------------

These utilities return Python dicts. Arrow output is not applicable
since their results are scalar or structural (not tabular).

StatisticsQueryUtility
~~~~~~~~~~~~~~~~~~~~~~

Query pre-computed statistics from an indexed trace file.

.. autoclass:: dftracer.utils.dftracer_utils_ext.StatisticsQueryUtility(runtime: Runtime | None = None)
   :members: process
   :undoc-members:

.. code-block:: python

   sq = StatisticsQueryUtility()
   result = sq.process("trace.pfw.gz", query_type="summary")
   print(result["total_events"])

   result = sq.process("trace.pfw.gz", query_type="top_n_names", top_n=5)
   for name, count in result["results"]:
       print(f"  {name}: {count}")

BloomQueryUtility
~~~~~~~~~~~~~~~~~

Query bloom filters in an index for fast event filtering.

.. autoclass:: dftracer.utils.dftracer_utils_ext.BloomQueryUtility(runtime: Runtime | None = None)
   :members: process
   :undoc-members:

.. code-block:: python

   bq = BloomQueryUtility()
   result = bq.process("trace.pfw.gz", predicates={"cat": ["POSIX"], "name": ["read"]})
   print(result["file_may_match"])
   print(result["candidate_checkpoints"])

StatisticsAggregatorUtility
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Aggregate statistics from a trace file via full scan.

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

ViewBuilderUtility
~~~~~~~~~~~~~~~~~~

Query the bloom-filter index to find candidate chunks matching
predicates.

.. autoclass:: dftracer.utils.dftracer_utils_ext.ViewBuilderUtility(runtime: Runtime | None = None)
   :members: process
   :undoc-members:

.. code-block:: python

   vb = ViewBuilderUtility()
   result = vb.process("trace.pfw.gz", predicates={"cat": ["POSIX"]})
   print(f"May match: {result['file_may_match']}")
   for c in result["candidates"]:
       print(f"  Checkpoint {c['checkpoint_idx']}: bytes {c['start_byte']}-{c['end_byte']}")

ReorganizationPlannerUtility
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Plan semantic reorganization of trace files.

.. autoclass:: dftracer.utils.dftracer_utils_ext.ReorganizationPlannerUtility(runtime: Runtime | None = None)
   :members: process
   :undoc-members:

.. code-block:: python

   rp = ReorganizationPlannerUtility()
   plan = rp.process(
       source_files=["trace1.pfw.gz", "trace2.pfw.gz"],
       groups=[{"name": "posix", "predicate": "cat=POSIX"}],
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
