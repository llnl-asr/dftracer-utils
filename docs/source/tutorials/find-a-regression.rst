:description: Build a baseline and a slower variant, then compare them to find where a per-category slowdown lives, in Python or C++.

Find a regression between two runs
====================================

This lesson builds two traces - a baseline and a slower variant - and compares
them to find where the slowdown lives. It assumes you have finished
:doc:`first-analysis`. It takes about five minutes.

Every step is shown in both **Python** and **C++**; pick your tab and follow
it top to bottom.

Create a baseline and a slower variant
-----------------------------------------

Both are the same shape as the lesson-1 trace (500 events, alternating
``POSIX``/``STDIO``), but the variant adds 150 microseconds to every
``POSIX`` event - a regression confined to one category, the kind a real
comparison needs to surface. Write both, in Python:

.. code-block:: python

   import gzip

   def write_trace(path, posix_penalty):
       with gzip.open(path, "wt") as f:
           for i in range(500):
               cat = "POSIX" if i % 2 else "STDIO"
               dur = 10 + i + (posix_penalty if cat == "POSIX" else 0)
               f.write(
                   f'{{"name":"read","cat":"{cat}","pid":1,"tid":1,'
                   f'"ts":{1000 + i},"dur":{dur},"ph":"X","args":{{}}}}\n'
               )

   write_trace("baseline.pfw.gz", posix_penalty=0)
   write_trace("variant.pfw.gz", posix_penalty=150)

You now have ``baseline.pfw.gz`` and ``variant.pfw.gz``: same event counts and
the same ``STDIO`` durations, but every ``POSIX`` event in the variant is 150
microseconds slower.

Index both
-----------

Indexing is idempotent and keyed by each trace's own path, so building it for
both files is one call each - see :doc:`../guides/core/indexing` for the full
picture.

.. code-block:: python

   import dftracer.utils as dft

   with dft.Indexer(files=["baseline.pfw.gz", "variant.pfw.gz"]) as ix:
       ix.ensure_indexed()

Compare them
-------------

.. tab-set::

   .. tab-item:: Python

      ``TraceViewer.compare(other)`` returns a lazy plan that aggregates both
      sides with the plan you set on the baseline (``group_by`` + ``agg``) and
      joins the results on the group key; ``collect()`` runs both scans. The
      baseline is the side the plan is set on; the argument is the variant:

      .. code-block:: python

         from dftracer.utils import TraceViewer

         baseline = (
             TraceViewer("baseline.pfw.gz")
             .group_by("cat")
             .agg("count", "mean:dur")
         )
         cmp = baseline.compare(TraceViewer("variant.pfw.gz")).collect()

   .. tab-item:: C++

      ``CompareView`` (``dftracer/utils/trace/comparator/compare_view.h``)
      wraps a baseline and a variant ``View``; ``group_by``/``agg`` set the
      shared aggregation plan and ``collect()`` runs both scans and returns the
      comparison ``DataFrame``.

      .. code-block:: cpp

         #include <dftracer/utils/trace/comparator/compare_view.h>
         #include <dftracer/utils/trace/views/view.h>

         using namespace dftracer::utils::trace;

         auto cmp = comparator::CompareView::of(
                        views::View::from_file("baseline.pfw.gz"),
                        views::View::from_file("variant.pfw.gz"))
                        .group_by({views::GroupKey::cat()})
                        .agg({{views::AggOp::Count, "", "count"},
                              {views::AggOp::Mean, "dur", "mean_dur"}})
                        .collect()
                        .get();  // blocks; a dataframe::DataFrame

Read the regression columns
-------------------------------

The comparison keeps the group key (``cat``) and, for each aggregate, adds an
``l_<m>`` / ``r_<m>`` pair (left = baseline, right = variant) plus the derived
``delta_<m>`` (variant minus baseline) and ``pct_<m>`` (percent change). With
``count`` and ``mean_dur`` as the aggregates, that is ``l_count``,
``r_count``, ``l_mean_dur``, ``r_mean_dur``, ``delta_count``, ``pct_count``,
``delta_mean_dur``, ``pct_mean_dur``.

Print it (``cmp.to_pandas()`` in Python; walk ``cmp.column(...)`` by name in
C++, as in lesson 1) and you get, for each group:

.. list-table::
   :header-rows: 1
   :widths: 14 12 12 16 16 14 14 18 18

   * - cat
     - l_count
     - r_count
     - l_mean_dur
     - r_mean_dur
     - delta_count
     - pct_count
     - delta_mean_dur
     - pct_mean_dur
   * - POSIX
     - 250
     - 250
     - 260.0
     - 410.0
     - 0
     - 0.0
     - 150.0
     - 57.69
   * - STDIO
     - 250
     - 250
     - 259.0
     - 259.0
     - 0
     - 0.0
     - 0.0
     - 0.0

``POSIX`` carries the regression: ``delta_mean_dur`` is +150 microseconds, a
57.69% increase; ``STDIO`` is untouched, so both its deltas are zero.
``delta_count`` and ``pct_count`` are computed too since ``count`` is numeric
- here they are zero because the penalty did not change how many events fall
in each group.

This comparison DataFrame has no statistical-significance or pass/fail
column: it is baseline/variant deltas, nothing more. For a threshold-based
"only show me changes above N%" report, or a hierarchical breakdown, reach for
the ``dftracer_comparator`` CLI (``--threshold``, ``--group-by``, ``--format
json``) - see :doc:`../guides/analysis/comparison` for the full flag
reference and what a hierarchical comparison adds.

What you learned
------------------

- Build a comparison by aggregating a **baseline** and a **variant** the same
  way and joining on the group key: ``TraceViewer.compare(other)`` in Python,
  ``CompareView`` in C++.
- The baseline's ``group_by``/``agg`` plan drives both sides; the result
  carries ``l_<m>`` / ``r_<m>`` per metric plus derived ``delta_<m>`` and
  ``pct_<m>``, including for count-like metrics.
- A zero delta means that group did not change between runs; a nonzero one
  tells you where to look next.
- ``dftracer_comparator`` covers the same ground from the command line, with a
  ``--threshold`` to filter small changes and a hierarchical config for
  nested groups.
