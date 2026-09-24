:description: Turn genesis sweep traces into one trace of per-call-path duration and counter distributions with dftracer_genesis_gen_dist, then query it from Python.

Build per-function distributions from genesis traces
====================================================

.. admonition:: Goal
   :class: goal

   Turn every run of a genesis trace sweep into one ``.pfw.gz`` file that
   holds, for each function call path, the distribution of its call
   durations and of every counter attributed to its calls, pooled over all
   processes and nodes of the run. Then load that file with ``TraceViewer``.

Command line
------------

.. code-block:: bash

   dftracer_genesis_gen_dist ~/traces/genesis-wisdom -o genesis.pfw.gz

   # Several roots into one file, fewer concurrent runs to save memory
   dftracer_genesis_gen_dist ./laghos ./minife -o genesis.pfw.gz \
       --executor-threads 4

The binary walks each root for ``nodes_<N>/ppn_<M>`` run directories. It
reads the matrix layout (``summary.json`` with a ``sets`` list and one
``compacted/<set>.pfw.gz`` per PAPI set) and the tioga layout, whose
``-<k>`` time slices mix one execution per PAPI set. With a tioga
``summary.json``, each process is matched to a set by its PAPI counters;
without one, each distinct counter list is a set named by its counters
joined with ``+``. ``unique_input`` is the input directory name. ``-o``
must end with ``.pfw.gz``.

A run that cannot be used is skipped with one line on stderr naming the run
directory, the file and the reason, for example a truncated ``.pfw.gz``, a
process without its ``start`` or ``end`` event, missing time slices or a
child call that ends after its parent. The other runs are still written and
the exit status is 1.

Query the output
----------------

.. code-block:: python

   from dftracer.utils import Indexer
   from dftracer.utils.trace_viewer import TraceViewer

   # Build the index once: it holds the run dictionary.
   with Indexer(files=["genesis.pfw.gz"]) as ix:
       ix.ensure_indexed()

   tv = TraceViewer("genesis.pfw.gz")

   # One record per call path per run: nested stats are args columns, and
   # the run's keys resolve through the index's run dictionary.
   dist = tv.select(
       "resolved.run.app", "resolved.run.system",
       "resolved.run.unique_input", "resolved.run.nodes",
       "resolved.run.ppn", "resolved.run.papi_set",
       "path", "depth", "count", "dur.p50", "dur.p99",
       "counters.PAPI_TOT_CYC.p50",
   ).collect()

   # Filter by run key: the filter becomes a test on the run id.
   laghos = tv.query('resolved.run.app == "laghos"').collect()

   # One RUN line per run with the run keys and the full summary.json.
   runs = TraceViewer("genesis.pfw.gz").phase("metadata").filter(
       "name == 'RUN'"
   ).collect()

Each record is a ``ph:3`` event with ``pid`` 0. Its ``args`` hold the
``run`` id, ``path`` (call names joined by ``;``), ``parent``, ``depth``,
``count``, a ``dur`` object and a ``counters`` object. Every statistic
object has ``min``, ``max``, ``avg``, ``p25``, ``p50``, ``p75``, ``p90`` and
``p99``, plus ``sum`` for counts. Counter entries also carry ``scope``
(``pid`` or ``host``) and ``kind`` (``delta`` or ``gauge``). The ``run`` id
is the FNV-1a hash of ``app|system|unique_input|nodes|ppn|papi_set``, so the
same run gets the same id in any output and files from separate invocations
can be concatenated. The run keys themselves are on the ``RUN`` line only:
the index detects the file as the ``genesis`` record schema and builds a
``run`` dictionary from the ``RUN`` lines, so ``resolved.run.<key>`` reads
``app``, ``system``, ``unique_input``, ``nodes``, ``ppn``, ``papi_set``,
``method``, ``sketch_accuracy`` or ``leaf`` on any record. Numeric keys read
as text (``"4"``) and match numeric filters (``resolved.run.nodes == 4``).

How counters are attributed
---------------------------

Counters are sampled every 100 ms (PAPI) to 1 s (host metrics) while most
calls last well under that, so values are attributed rather than measured
per call:

- PAPI deltas are pro-rated by the time a call overlaps each sample
  interval. Every call inside one sample gets the same rate, so per-call
  ratios such as instructions per cycle are the sample's ratio.
- Host network and disk deltas are split equally over the ranks of the host
  and then pro-rated, so their ``sum`` equals the host total.
- CPU percentages, memory levels and GPU power, utilization and memory are
  time-weighted means over the call and have no ``sum``. Per-core CPU
  counters are left out; the node-wide ``cpu`` counter is kept.
- Matrix CUDA API events carry a clock that is not aligned with the trace,
  so they appear as depth-0 records with durations only.

Percentiles come from a mergeable sketch with 1% relative accuracy, stated
as ``sketch_accuracy`` in each ``RUN`` line.
