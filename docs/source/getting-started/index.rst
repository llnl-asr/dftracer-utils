:description: Start here: install dftracer-utils, run the 30-second quickstart, and move on to the graded tutorials.

Get started
===========

New to dftracer-utils? Install it and run the 30-second quickstart here, then
head to the :doc:`Tutorials <../tutorials/index>` for a hand-held path from your
first analysis to writing your own engine plugin.

.. grid:: 1 1 2 2
   :gutter: 3

   .. grid-item-card:: :octicon:`download` Installation
      :link: installation
      :link-type: doc

      Install the Python package, or build the C/C++ libraries from source with
      the CMake presets.

   .. grid-item-card:: :octicon:`zap` Quickstart
      :link: ../quickstart
      :link-type: doc

      The 30-second version: point at a directory of traces and read a result
      back.

Next
----

When the quickstart runs, start the graded :doc:`../tutorials/index`:

- :doc:`../tutorials/first-analysis` - your first end-to-end analysis.
- :doc:`../tutorials/analysis-in-depth` - derived columns, duql filters, and
  DataFrame/Series operations.
- :doc:`../tutorials/extending-the-engine` - author a plugin over the fused scan.

.. toctree::
   :hidden:
   :maxdepth: 1

   installation
   ../quickstart
