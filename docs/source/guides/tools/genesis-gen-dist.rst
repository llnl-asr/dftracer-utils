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
must end with ``.pfw.gz``. The output is multi-member gzip: a member ends at
the first line end after ``--member-size`` uncompressed bytes (the checkpoint
size, 32 MiB, by default; units such as ``512KB`` are accepted), so a large
run spreads over several members that readers decode in parallel.

Memory
~~~~~~

The tool holds at most ``--memory-budget`` bytes (default: a third of the
memory available to the process, so a cgroup limit is respected) however large
a run is. An eighth of it goes to the gzip writer, and the rest is shared by
the runs that are processed at the same time, as long as each has a share of
at least 64 MB. Within a run, the decoded calls stay in memory up to their
share; the rest go to a temporary file, are sorted there in runs of the
share's size, and are merged back in order. Each worker writes a run as soon
as it is done, and the members are compressed and written in parallel, so no
run waits for another. The output holds the same lines whatever the budget
and the thread count, but not in the same order: runs, and the members of one
run, appear in the order they finish.

The temporary file goes to ``DFTRACER_UTILS_SPILL_DIR`` when it is set. When it
is not, the tool picks the writable node-local disk mount with the most free
space, never a RAM-backed or network one. When no mount root is writable by
you, it uses a per-user directory under ``/var/tmp`` or ``~/.cache`` on a local
disk, and as a last resort the system temp directory, with a warning when that
is RAM-backed. The file is removed as soon as it is created, so
nothing is left behind.

.. code-block:: bash

   # Keep the tool under 64 GB; name the spill directory only to override it
   dftracer_genesis_gen_dist ./laghos -o genesis.pfw.gz --memory-budget 64GB

A spill directory that sits in memory counts against the memory limit, so
only set ``DFTRACER_UTILS_SPILL_DIR`` to a disk. Memory beyond the budget is still used by the process itself (threads,
libraries, I/O buffers, about 50 MB), by the output lines of each run being
written, and by the decoded size of one gzip member per open reader: a
trace written as a single huge gzip member is decoded whole, so split it first
with ``dftracer_split``.

A run whose counter series or path records alone exceed its share is skipped
with a reason that names the share, and the exit status is 1. Raise
``--memory-budget`` for it.

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

   # Build the index once: it stores the runs row set.
   with Indexer(files=["genesis.pfw.gz"]) as ix:
       ix.ensure_indexed()
       runs = ix.rowset("runs")  # one row per run, with the run keys

   tv = TraceViewer("genesis.pfw.gz")

   # Call paths whose median duration is above 1000 us.
   slow = tv.duql('where gtype == "func" and dur.p50 > 1000').collect()

   # One filter over every counter of every call path.
   idle = tv.duql(
       'where gtype == "counter" and metric == "cpu.idle_pct" and v.p50 > 50'
   ).collect()

   # Read run keys on any record through the runs row set.
   laghos = tv.duql(
       'derive app = run -> runs.app | where app == "laghos"'
   ).collect()

   # The runs row set as query rows, with no trace decoded.
   runs = tv.duql("from runs").collect()

The output is a long format. Every line has a ``gtype``, the genesis record type, and a ``run`` id and
none has the dftracer fields ``ph``, ``pid``, ``tid`` or ``args``. The
``run`` id is the FNV-1a hash of
``app|system|unique_input|nodes|ppn|papi_set``, so the same run gets the
same id in any output and files from separate invocations can be
concatenated. There are three kinds of line:

- ``run``, one per run: ``version`` (the format version, 1), ``run``, ``app``, ``system``, ``unique_input``,
  ``nodes``, ``ppn``, ``papi_set``, ``method``, ``sketch_accuracy``,
  ``leaf`` and ``summary``, the run's ``summary.json`` when it has one.
- ``func``, one per call path: ``run``, ``path`` (call names joined by
  ``;``), ``parent`` (absent at the root), ``name``, ``cat``, ``depth``,
  ``ts`` (the earliest call start), ``count`` and ``dur``.
- ``counter``, one per call path and counter: ``run``, ``path``, ``name``
  (the call name), ``metric`` (the counter name), ``scope`` (``pid`` or
  ``host``), ``kind`` (``delta`` or ``gauge``) and ``v``.

A metric name is a value in ``metric``, never a JSON key, so the number of
JSON paths does not grow with the number of counters. ``dur`` has ``min``,
``max``, ``sum``, ``avg``, ``p25``, ``p50``, ``p75``, ``p90``, ``p99`` and
``sketch``. ``v`` has ``n`` (the calls that received a value), ``min``,
``max``, ``avg``, the same percentiles and ``sketch``, plus ``sum`` for
``delta`` counters only. Every record carries its ``run``, so a reader groups
records by it and does not depend on their order in the file.

``sketch`` is the DDSketch behind the percentiles, as base64 text of its
serialized form. A reader can decode and merge the sketches of several
records to get exact quantiles over their union; in C++ use
``BasicDDSketch`` deserialize. Merging stored sketches in a query is not
supported yet.

The index detects a file whose records carry ``gtype`` and ``run`` as the
``genesis`` record schema. Its source declares ``data`` as every record but
the ``run`` lines, so a query with no ``from`` reads ``func`` and
``counter`` records. It also declares the row set ``runs``, built from the
``run`` lines and stored by the index build, so ``run -> runs.<key>`` reads
``app``, ``system``, ``unique_input``, ``nodes``, ``ppn``, ``papi_set``,
``method``, ``sketch_accuracy`` or ``leaf`` on any record and ``from runs``
decodes no trace. Numeric keys stay numbers (``run -> runs.nodes == 4``).
``from all | where gtype == "run"`` returns the ``run`` records with
``summary``, whose fields are ``summary.*`` columns.

Files written by an earlier version, with ``ph:3`` events and a ``counters``
object, are not read as ``genesis``; they index as ``dftracer``. Regenerate
them.

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
as ``sketch_accuracy`` in each ``run`` line.
