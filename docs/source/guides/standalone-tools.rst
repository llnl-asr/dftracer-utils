:description: Command-line tools and building blocks for preparing, replaying and generating traces.

Standalone tools
================

Command-line tools and building blocks for preparing, replaying and generating traces.

.. grid:: 1 1 2 2
   :gutter: 3

   .. grid-item-card:: Compress and split traces
      :link: io/compression
      :link-type: doc

      Write multi-member gzip or split a trace into chunks.

   .. grid-item-card:: Replay a trace
      :link: tools/replay
      :link-type: doc

      Re-run the recorded I/O for real or as timed sleeps.

   .. grid-item-card:: Generate a DLIO config
      :link: tools/dlio-config
      :link-type: doc

      Fit timing distributions from traces into a DLIO config.

   .. grid-item-card:: Distributions from genesis
      :link: tools/genesis-gen-dist
      :link-type: doc

      Turn genesis sweeps into per-call-path distributions.

   .. grid-item-card:: Control log output
      :link: tools/logging
      :link-type: doc

      Set the log level and color from Python, the CLI or C++.

   .. grid-item-card:: Utility building blocks
      :link: ../utilities
      :link-type: doc

      File I/O, filesystem, hashing and reader components.

.. toctree::
   :hidden:

   io/compression
   tools/replay
   tools/dlio-config
   tools/genesis-gen-dist
   tools/logging
   ../utilities
