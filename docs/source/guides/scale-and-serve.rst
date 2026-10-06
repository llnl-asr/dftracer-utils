:description: Run across ranks and dask workers, and serve indexed traces over HTTP.

Scale out and serve
===================

Run across ranks and dask workers, and serve indexed traces over HTTP.

.. grid:: 1 1 2 2
   :gutter: 3

   .. grid-item-card:: Run across ranks with MPI
      :link: scale/mpi
      :link-type: doc

      Distribute flamegraph, counter and aggregate runs over ranks.

   .. grid-item-card:: Index on a dask cluster
      :link: scale/distributed-index
      :link-type: doc

      Build index shards in parallel on workers.

   .. grid-item-card:: Combine aggregations
      :link: scale/distributed-aggregation
      :link-type: doc

      Merge per-rank partials without a rescan.

   .. grid-item-card:: Partitioned DataFrames
      :link: scale/distributed-frame
      :link-type: doc

      Map, combine, shuffle and join across partitions.

   .. grid-item-card:: The HTTP server
      :link: ../server
      :link-type: doc

      Run dftracer_server over an indexed trace directory.

   .. grid-item-card:: Query over HTTP
      :link: serving/http-server
      :link-type: doc

      Query the index from any HTTP client.

   .. grid-item-card:: The visualization API
      :link: serving/viz-api
      :link-type: doc

      Drive the /api/viz routes for your own plots.

.. toctree::
   :hidden:

   scale/mpi
   scale/distributed-index
   scale/distributed-aggregation
   scale/distributed-frame
   ../server
   serving/http-server
   serving/viz-api
