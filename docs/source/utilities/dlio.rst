DLIO Config Generation
======================

The ``dlio`` utilities power the ``dftracer_gen_dlio_config`` binary
(see :ref:`dftracer_gen_dlio_config <cli-shared-flags>` in the CLI reference).
They consume an already-populated ``AGGREGATION`` column family (produced by
:doc:`/cli` ``dftracer_aggregator`` or by the shared
``aggregation_runner`` library function) and emit a DLIO-compatible YAML
config describing per-component timing distributions.

.. code-block:: cpp

   #include <dftracer/utils/utilities/dlio/barrier_simulator.h>
   #include <dftracer/utils/utilities/dlio/optimizer.h>
   #include <dftracer/utils/utilities/dlio/statistic.h>
   #include <dftracer/utils/utilities/dlio/trace_loader.h>
   #include <dftracer/utils/utilities/dlio/worker_queue.h>
   #include <dftracer/utils/utilities/dlio/yaml_emit.h>

Pipeline overview
-----------------

End-to-end the module composes four pieces:

1. **trace_loader** opens an existing RocksDB read-only (with the AGGREGATION
   merge operator re-attached), iterates the ``AGGREGATION`` column family,
   groups entries by ``(cat, name, pid, time_bucket)``, and synthesizes a flat
   per-rank sample stream per component (``fetch.block``, ``fetch.iter``,
   ``preprocess``, ``item``). Sketches are used for inverse-CDF sampling when
   the aggregator was run with ``--compute-percentiles``; otherwise the
   per-call mean is replicated.

2. The :doc:`distribution fitter <common>` (under
   ``common/statistics/distributions.h`` and ``mixture.h``) fits the lowest-BIC
   model from {Normal, Lognormal, Gamma, Exponential, Weibull, GMM-2, GMM-3}.

3. **BarrierSimulator** simulates one DLIO training run across the captured
   ranks/steps using the fitted distribution as the ``fetch.block`` sampler.
   It produces an end-to-end duration, rank variance, and a Kolmogorov-Smirnov
   similarity between simulated and trace ``fetch.block`` samples.

4. **optimizer** runs a sequential momentum loop tuning the ``max_bound``
   percentile used to clamp the sampler, minimizing simulator E2E error while
   keeping the CDF similarity above a target.

5. **yaml_emit** renders the final YAML.

trace_loader
------------

.. code-block:: cpp

   TraceLoaderOptions opts;
   opts.max_samples_per_entry = 100;   // cap per (pid, bucket) entry, 0 = unlimited
   opts.seed = 0xD15710;                // seed for inverse-CDF sampling

   AggregatedTraces traces = load_aggregated_traces(db_path, opts);

   if (!traces.any_data) { /* no DLIO events in this DB */ }
   if (!traces.sketches_available) {
       // The aggregator was run without --compute-percentiles. We fell back to
       // mean replication; rerun with the flag for higher-fidelity output.
   }

Returns an ``AggregatedTraces`` with:

- ``fetch_block_trace`` / ``fetch_iter_trace`` / ``getitem_trace`` -
  per-rank ``std::vector<std::vector<double>>`` of seconds, in pid-ascending
  then time-bucket-ascending order.
- ``computation_times`` / ``preprocess_times`` - flat sample arrays in seconds
  (the input to ``fit_all_single_distributions``).
- ``fetch_block_stats`` / ``fetch_iter_stats`` / ``preprocess_stats`` /
  ``getitem_stats`` - :doc:`Statistic <common>` objects, with merged DDSketches
  attached when available.
- ``trace_e2e_duration`` and per-component ``ComponentTimeMetrics`` with both
  ``accumulated_time`` (sum of ``count x mean``) and ``union_time`` (true
  wall-clock union via ``sweep_union`` over per-entry ``(ts, te)`` boundaries).

BarrierSimulator
----------------

.. code-block:: cpp

   BarrierSimulatorContext ctx = make_simulator_context(
       traces, /*num_workers=*/8, /*prefetch_factor=*/2);

   auto sampler = make_sampler(fitted_model);  // from common/statistics
   BarrierSimulator sim;
   BarrierSimulationResult result = sim.simulate(
       ctx, /*base_seed=*/42, sampler);

   printf("e2e=%.3fs error=%.2f%% fetch_block_cdf_sim=%.4f\n",
          result.e2e_duration,
          result.e2e_error * 100.0,
          result.fetch_block_cdf_similarity);

Free helpers exposed alongside ``BarrierSimulator``:

- ``sweep_union(boundaries)`` - sweep-line interval union, microseconds to
  seconds.
- ``cdf_similarity(a, b)`` - ``1 - KS`` between two empirical samples.
- ``variance(values)`` - population variance.

Distribution fitting
--------------------

Lives under ``common/statistics`` and works on any sample array, not just DLIO
traces - see :doc:`common` for ``FittedDistribution``, ``FittedMixture``,
``BestModel`` (the ``std::variant``), ``select_best_model``, ``make_sampler``,
and free ``pdf`` / ``cdf`` / ``quantile`` overloads.

optimizer
---------

.. code-block:: cpp

   OptimizerOptions opt_opts;
   opt_opts.max_iterations = 5;
   opt_opts.target_e2e_error = 0.05;
   opt_opts.target_cdf_similarity = 0.90;
   opt_opts.patience = 10;
   opt_opts.epsilon = 1.0;
   opt_opts.momentum = 0.9;
   opt_opts.min_percentile = 50.0;
   opt_opts.initial_percentile = 95.0;
   opt_opts.base_seed = 42;

   OptimizerResult opt = optimize_max_bound_percentile(
       ctx, fitted_model, traces.computation_times, opt_opts);

   double max_bound = percentile(sorted_samples, opt.best_percentile);

Each iteration constructs a fresh sampler clamped at
``percentile(sample_times, current_percentile)``, runs ``simulate()``, and
adjusts ``current_percentile`` by a momentum-smoothed step proportional to the
E2E error sign. Convergence: ``e2e_error < target_e2e_error`` AND
``fetch_block_cdf_similarity > target_cdf_similarity``. Early-stops after
``patience`` iterations without improvement.

yaml_emit
---------

.. code-block:: cpp

   DlioTimingBlock comp{best_comp_model, comp_max_bound};
   DlioTimingBlock prep{best_prep_model, prep_max_bound};

   std::ofstream out("dlio_config.yaml");
   write_dlio_yaml(out, &comp, &prep);

   // Or render to a string:
   std::string yaml = render_dlio_yaml(&comp, &prep);

Renders both single distributions and mixtures into the DLIO schema
(``type: <family>`` + family-specific params, or ``type: mixture`` +
``n_components`` + ``components: [{weight, params: {...}}]``). Pass ``nullptr``
to either block argument to omit it.
