:description: Turn a trace directory into a DLIO Benchmark config with timing distributions fitted from the traces, using dftracer_gen_dlio_config.

Generate a DLIO config from traces
==================================

.. admonition:: Goal
   :class: goal

   Turn a directory of traces into a `DLIO Benchmark
   <https://github.com/argonne-lcf/dlio_benchmark>`_ training config, with the
   computation and preprocess timing distributions fitted to what the traces
   actually did. ``dftracer_gen_dlio_config`` runs the whole pipeline: it loads and
   aggregates the traces, simulates the data-loader barrier to refine a percentile
   bound, and emits the YAML.

This is CLI-primary. The pipeline underneath is a C++ library (namespace
``dftracer::utils::utilities::dlio``); there is no Python binding.

Command line
------------

.. code-block:: bash

   # Minimal: read ./traces, write dlio.yaml
   dftracer_gen_dlio_config ./traces -o dlio.yaml

   # Tune the simulator: more iterations, tighter convergence
   dftracer_gen_dlio_config ./traces -o dlio.yaml \
       --simulation-iterations 20 --target-e2e-error 0.02

``-o``/``--output`` is required. ``-d``/``--directory`` defaults to ``.`` and
takes ``.pfw.gz`` traces.

Key flags
~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 34 16 50

   * - Flag
     - Default
     - Meaning
   * - ``-o``, ``--output``
     - (required)
     - Output path for the DLIO YAML config.
   * - ``--max-bound-percentile``
     - ``95.0``
     - Initial ``max_bound`` percentile (0-100).
   * - ``--simulation-iterations``
     - ``5``
     - Max simulator iterations for percentile refinement.
   * - ``--target-e2e-error``
     - ``0.05``
     - Target relative end-to-end error to declare convergence.
   * - ``--target-cdf-similarity``
     - ``0.90``
     - Target fetch_block CDF similarity.
   * - ``--patience``
     - ``10``
     - Early-stop after this many iterations without improvement.
   * - ``--min-percentile``
     - ``50.0``
     - Floor on the ``max_bound`` percentile.
   * - ``--num-workers``
     - ``8``
     - DataLoader worker count for the simulator.
   * - ``--prefetch-factor``
     - ``2``
     - DataLoader prefetch factor.
   * - ``-t``, ``--time-interval``
     - ``5000.0``
     - Aggregation time interval in ms.
   * - ``--event-map``
     - ``""``
     - YAML or JSON file remapping the ``(cat, name)`` of the ``fetch_block`` /
       ``fetch_iter`` / ``preprocess`` / ``item`` components.

The optimizer also accepts ``--epsilon``, ``--momentum``, ``--seed``,
``--max-samples-per-entry``, and simulator seed controls; run
``dftracer_gen_dlio_config --help`` for the full list.

Remapping event names
---------------------

The pipeline recognizes four components by their ``(category, name)``:
``fetch_block`` (default ``dataloader`` / ``fetch.block``), ``fetch_iter``
(``dataloader`` / ``fetch.iter``), ``preprocess`` (``data`` / ``preprocess``),
and ``item`` (``data`` / ``item``). If your traces use different names, point
``--event-map`` at a YAML or JSON file that remaps them.

What it emits
-------------

The generated YAML carries the fitted timing blocks, shaped as
``train.computation_time`` and ``reader.preprocess_time``, each with a
distribution ``type`` and a ``max_bound`` (in seconds) chosen by the barrier
simulator. Feed the file straight to DLIO Benchmark.

See also
--------

- :doc:`../../trace-viewer` - inspect the timing distributions the config is
  fitted to.
- :doc:`replay` - replay the same traces instead of modeling them.
