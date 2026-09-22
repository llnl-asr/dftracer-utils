:description: Compute percentiles, histograms, and fitted distributions over trace durations from a DDSketch built during the scan.

Distributions, percentiles, and sketches
=========================================

Beyond counts and means: percentiles, histograms, and fitted distributions over
trace durations (or any numeric field). Percentiles and histograms come from a
DDSketch computed during the scan, so they are exact to a bounded relative
accuracy without buffering every value.

Percentiles and histograms from a query
---------------------------------------

The trace aggregation surface exposes quantile and histogram aggregates
directly. A quantile aggregate reduces each group to the ``q``-quantile of a
field from a DDSketch; a histogram aggregate emits the raw sketch bins.

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         #include <dftracer/utils/trace/views/view.h>

         using namespace dftracer::utils::trace::views;

         auto df = View::from_file("trace.pfw.gz")
                       .group_by({GroupKey::cat()})
                       .agg({{AggOp::Pct, "dur", "p50", "", 0.50},
                             {AggOp::Pct, "dur", "p99", "", 0.99},
                             {AggOp::Hist, "dur", "hist"}})
                       .collect()
                       .get();

      The ``Pct`` spec's fifth field is the quantile ``q`` in (0,1). ``Hist``
      emits a ``list<struct<lo,hi,count>>`` column.

   .. tab-item:: Python

      .. code-block:: python

         from dftracer.utils import TraceViewer

         df = (TraceViewer("traces/")
               .group_by("cat")
               .agg("p50:dur", "p99:dur", "hist:dur")
               .collect()      # -> LazyFrame
               .collect())     # -> DataFrame

      ``"pNN:<field>"`` is percentile shorthand (``"p999:dur"`` is the 99.9th);
      ``"pct:<field>:<q>"`` takes an explicit quantile.

See :doc:`aggregation` for the full aggregate vocabulary.

From the CLI
-------------

``dftracer_stats`` reports the same bloom-accelerated statistics from a
pre-built ``.dftindex`` (auto-building one if missing) without decompressing
the trace:

.. code-block:: console

   dftracer_stats traces/ --report summary
   dftracer_stats traces/ --report detailed --group-by name --top-n 20
   dftracer_stats a.pfw.gz b.pfw.gz --report duration --json

``--report`` selects the report: ``summary``, ``categories``, ``names``,
``pid_tids``, ``time_range``, ``duration``, ``top-names``, ``top-categories``,
or ``detailed`` (per-operation duration and I/O distributions, grouped by
``--group-by``). Other flags: ``--top-n`` caps top-N results (``0`` = all),
``--top-n-pid-tid`` caps PID:TID pairs shown in the summary header,
``--group-by`` (``detailed`` only) takes one or more of ``name``, ``cat``,
``pid``, ``tid``, ``fhash``, ``hhash``, ``pid_tid`` for composite grouping,
``--filter-names`` / ``--filter-cats`` restrict which events are scanned,
``--json`` switches to machine-readable output, and ``--no-auto-index``
refuses to build a missing index instead of doing it implicitly. It shares the
directory/files, ``--index-dir``, ``--query``, and executor-thread flags
common to the other CLI tools; run ``dftracer_stats --help`` for the full list.

Whole-trace summary
-------------------

For a quick single-row summary without choosing group keys, Python's
``statistics()`` returns a dict of duration count, mean and standard deviation
plus the timestamp range.

.. tab-set::

   .. tab-item:: C++

      There is no ``statistics()`` shortcut in C++; aggregate the whole trace
      with an empty ``group_by`` (one output row).

      .. code-block:: cpp

         auto df = View::from_file("trace.pfw.gz")
                       .group_by({})
                       .agg({{AggOp::Count, "", "duration_count"},
                             {AggOp::Mean, "dur", "duration_mean_us"},
                             {AggOp::Std, "dur", "duration_stddev_us"},
                             {AggOp::Min, "ts", "min_timestamp_us"},
                             {AggOp::Max, "ts", "max_timestamp_us"}})
                       .collect()
                       .get();

   .. tab-item:: Python

      .. code-block:: python

         stats = TraceViewer("traces/").statistics()
         # {"duration_count", "duration_mean_us", "duration_stddev_us",
         #  "min_timestamp_us", "max_timestamp_us"}

Column-level statistics
-----------------------

Any ``Series`` (a column of a collected frame) carries the reducers directly, so
you can compute a percentile or moment on a result column without another scan.

.. tab-set::

   .. tab-item:: C++

      .. code-block:: cpp

         double p99 = df.column("dur").quantile(0.99);
         double sd  = df.column("dur").stddev();     // sample; stddev(false) for population
         double sk  = df.column("dur").skewness();
         double ku  = df.column("dur").kurtosis();

   .. tab-item:: Python

      .. code-block:: python

         p99 = df["dur"].quantile(0.99)
         sd  = df["dur"].stddev()          # stddev(sample=False) for population
         sk  = df["dur"].skewness()
         ku  = df["dur"].kurtosis()

See :doc:`../data/series` for the full reducer list.

Sketches and distribution fitting (C++)
---------------------------------------

The native statistics primitives live in
``dftracer/utils/utilities/common/statistics/`` (namespace
``dftracer::utils::utilities::common::statistics``). Use them to accumulate a
distribution incrementally or off a set of values.

``DDSketch`` - relative-accuracy quantile sketch, mergeable across threads:

.. code-block:: cpp

   #include <dftracer/utils/utilities/common/statistics/ddsketch.h>

   using namespace dftracer::utils::utilities::common::statistics;

   DDSketch sketch(0.01);          // 1% relative accuracy
   for (double v : durations) sketch.add(v);
   double p99 = sketch.quantile(0.99);   // NaN on an empty sketch
   // sketch.merge(other), sketch.bins(), sketch.serialize()

``DistinctSketch`` (``BasicDistinctSketch<Precision=13>``) - a mergeable
HyperLogLog++ distinct-count sketch, for counting a dimension left out of a
grouping key without keeping every value:

.. code-block:: cpp

   #include <dftracer/utils/utilities/common/statistics/distinct_sketch.h>

   using namespace dftracer::utils::utilities::common::statistics;

   DistinctSketch sketch;              // 8192 registers, ~1.15% std error dense
   for (auto h : file_hashes) sketch.add_hash(h);   // pre-hashed (fhash/hhash)
   sketch.add("libc.so");              // or hash a raw string
   std::uint64_t distinct = sketch.estimate();
   // sketch.merge_from(other)

It stays sparse (and exact) below ``REGISTERS / 4`` distinct values, then
promotes to a dense register array; two sketches only merge at the same
``Precision``.

``Log2Histogram`` - fixed power-of-two bins with an ASCII/block renderer:

.. code-block:: cpp

   #include <dftracer/utils/utilities/common/statistics/log2_histogram.h>

   Log2Histogram hist;
   hist.add(size_bytes);
   double p90 = hist.approx_percentile(0.90);
   std::string chart = hist.render_ascii(40, "bytes");

``distributions.h`` fits parametric distributions (Normal, Lognormal, Gamma,
Exponential, Weibull) and picks the best by KS statistic:

.. code-block:: cpp

   #include <dftracer/utils/utilities/common/statistics/distributions.h>

   std::vector<FittedDistribution> fits = fit_all_single_distributions(data);
   std::optional<FittedDistribution> best = best_fit_by_ks(fits);

These are C++ / native primitives. Python exposes the aggregate-level percentiles
and ``Series`` reducers above rather than the sketch objects; for a fitted
distribution from Python, collect the values and fit them with SciPy.

See also
--------

- :doc:`aggregation` for the query-level aggregate vocabulary.
- :doc:`../data/series` for column reducers and kernels.
- :doc:`comparison` for baseline-vs-variant regression on these stats.
