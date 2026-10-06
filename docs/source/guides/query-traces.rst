:description: Open traces, filter them with duql, keep an index fresh and run many reads over one scan.

Query traces
============

Open traces, filter them with duql, keep an index fresh and run many reads over one scan.

.. grid:: 1 1 2 2
   :gutter: 3

   .. grid-item-card:: Filter with duql
      :link: core/duql
      :link-type: doc

      Write filters and pipelines in duql from Python, C++ or text.

   .. grid-item-card:: Build and use an index
      :link: core/indexing
      :link-type: doc

      Build the .dftindex once so later queries skip decompression.

   .. grid-item-card:: Query with the View engine
      :link: analysis/views
      :link-type: doc

      Filter, aggregate, export or fold in one lazy pass.

   .. grid-item-card:: Many reads, one scan
      :link: analysis/sessions
      :link-type: doc

      Share one scan across several branches with a session.

   .. grid-item-card:: Diagnose a slow query
      :link: analysis/diagnosing-slow-queries
      :link-type: doc

      Find where the time goes and pick the knob that fixes it.

   .. grid-item-card:: Stream lines from a trace
      :link: io/reading-files
      :link-type: doc

      Read a gzip trace line by line at the lowest C++ level.

   .. grid-item-card:: Explore in the web UI
      :link: ../trace-viewer
      :link-type: doc

      Browse traces in the viewer that dftracer_server serves.

.. toctree::
   :hidden:

   core/duql
   core/indexing
   analysis/views
   analysis/sessions
   analysis/diagnosing-slow-queries
   io/reading-files
   ../trace-viewer
