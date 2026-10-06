:description: Roll events up, measure distributions, compare runs and hand results to other tools.

Analyze traces
==============

Roll events up, measure distributions, compare runs and hand results to other tools.

.. grid:: 1 1 2 2
   :gutter: 3

   .. grid-item-card:: Aggregate events
      :link: analysis/aggregation
      :link-type: doc

      Group events by keys and reduce each group.

   .. grid-item-card:: Distributions and percentiles
      :link: analysis/statistics
      :link-type: doc

      Percentiles, histograms and fitted distributions from sketches.

   .. grid-item-card:: Compare two runs
      :link: analysis/comparison
      :link-type: doc

      Join a baseline and a variant and read the deltas.

   .. grid-item-card:: Get data out
      :link: analysis/export
      :link-type: doc

      Move results to pandas, polars, Arrow, NumPy or Parquet.

   .. grid-item-card:: Use the dfanalyzer bridge
      :link: tools/dfanalyzer
      :link-type: doc

      Feed dfanalyzer the frames it expects.

.. toctree::
   :hidden:

   analysis/aggregation
   analysis/statistics
   analysis/comparison
   analysis/export
   tools/dfanalyzer
