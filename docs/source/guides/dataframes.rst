:description: The columnar engine behind every result: build, compute, join and reshape columns natively.

DataFrames and Series
=====================

The columnar engine behind every result: build, compute, join and reshape columns natively.

.. grid:: 1 1 2 2
   :gutter: 3

   .. grid-item-card:: Work with DataFrames
      :link: data/dataframe
      :link-type: doc

      Build or import a frame, then project, filter and sort it.

   .. grid-item-card:: Plans with LazyFrame
      :link: data/lazyframe
      :link-type: doc

      Build a deferred plan and stream it under a memory budget.

   .. grid-item-card:: Work with Series
      :link: data/series
      :link-type: doc

      Reducers, element-wise math and scans on one typed column.

   .. grid-item-card:: pandas and polars surfaces
      :link: data/pandas-polars
      :link-type: doc

      Use pandas or polars habits on native frames.

   .. grid-item-card:: Ingest and interoperate
      :link: data/ingest
      :link-type: doc

      Import from and export to pandas, polars, Arrow and NumPy.

   .. grid-item-card:: Compute and reshape columns
      :link: core/columnar-ops
      :link-type: doc

      Derive and transform columns inside the SIMD engine.

   .. grid-item-card:: Join frames
      :link: data/joins
      :link-type: doc

      Inner, outer, semi, anti, cross and as-of joins on keys.

   .. grid-item-card:: Roll up over time windows
      :link: data/time-windows
      :link-type: doc

      Bucket rows by time and aggregate each window.

   .. grid-item-card:: Reshape frames
      :link: data/reshape
      :link-type: doc

      Pivot, unpivot, explode and unnest without changing values.

   .. grid-item-card:: How the engine works
      :link: ../columnar-engine
      :link-type: doc

      Typed columns, their encodings and the kernels that read them.

.. toctree::
   :hidden:

   data/dataframe
   data/lazyframe
   data/series
   data/pandas-polars
   data/ingest
   core/columnar-ops
   data/joins
   data/time-windows
   data/reshape
   ../columnar-engine
