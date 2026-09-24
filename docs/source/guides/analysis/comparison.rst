:description: Quantify a regression: aggregate two runs the same way, equi-join the groups, and read off per-metric delta and percent-change columns.

Compare a baseline against a variant
====================================

Quantify a regression: aggregate two runs the same way, line the groups up, and
read off the per-metric delta. Both sides run their own pruned scan; the results
are equi-joined on the shared group key, then each numeric metric ``m`` gets a
``delta_<m>`` (variant minus baseline) and ``pct_<m>`` (percent change) column.

Compare two views
-----------------

.. tab-set::

   .. tab-item:: C++

      ``CompareView`` (``dftracer/utils/trace/comparator/compare_view.h``) wraps
      two ``View`` sources; ``group_by``/``agg`` set the shared aggregation and
      ``collect()`` returns the comparison ``DataFrame``.

      .. code-block:: cpp

         #include <dftracer/utils/trace/comparator/compare_view.h>
         #include <dftracer/utils/trace/views/view.h>

         using namespace dftracer::utils::trace;

         auto cmp = comparator::CompareView::of(
                        views::View::from_file("baseline.pfw.gz"),
                        views::View::from_file("variant.pfw.gz"))
                        .group_by({views::GroupKey::cat()})
                        .agg({{views::AggOp::Count, "", "count"},
                              {views::AggOp::Mean, "dur", "dur"}})
                        .collect()
                        .get();

   .. tab-item:: Python

      ``TraceViewer.compare(other)`` compares against another viewer and
      returns a lazy ``LazyFrame``; ``collect()`` runs it. The baseline must
      already carry a ``group_by`` + ``agg`` plan; that plan drives both sides.

      .. code-block:: python

         from dftracer.utils import TraceViewer

         baseline = TraceViewer("baseline.pfw.gz").group_by("cat").agg("count", "mean:dur")
         cmp = baseline.compare(TraceViewer("variant.pfw.gz")).collect()

The result carries the group key columns, an ``l_<m>`` / ``r_<m>`` pair for each
metric (left = baseline, right = variant), and the derived ``delta_<m>`` and
``pct_<m>``. Comparing a run against itself yields all-zero deltas.

From the command line
---------------------

``dftracer_comparator`` runs the comparison over files or directories and prints
a table (or JSON). It supports a hierarchical config for nested metric groups.

.. code-block:: console

   $ dftracer_comparator --baseline baseline/ --variant variant/ \
       --group-by cat,name --threshold 5 --format table

Verified options:

.. list-table::
   :header-rows: 1
   :widths: 32 68

   * - Option
     - Meaning
   * - ``--baseline`` / ``--variant``
     - Baseline / variant trace file or directory
   * - ``--baseline-index-dir`` / ``--variant-index-dir``
     - Index directory for each side (default: co-located with the data)
   * - ``--group-by``
     - Comma-separated group keys (default: ``cat,name``)
   * - ``--format``
     - ``table`` (default) or ``json``
   * - ``-t`` / ``--time-interval``
     - Time bucket width in milliseconds (default: 5000)
   * - ``--threshold``
     - Hide changes below this percentage
   * - ``--config``
     - JSON config file for a hierarchical comparison
   * - ``--preset``
     - Built-in preset (supported: ``dlio``)
   * - ``--no-color`` / ``--compact``
     - Disable ANSI color / compact output

See also
--------

- :doc:`aggregation` for the ``group_by`` / ``agg`` vocabulary both sides share.
- :doc:`../data/joins` for the join underneath the comparison.
- :doc:`statistics` for the per-metric distributions being compared.
