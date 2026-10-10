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
run spreads over several members that readers decode in parallel. Each worker
fills one member at a time from the writer's eighth of the budget, so the
member size is lowered, to no less than 1 MiB, until a member is at most a
quarter of a worker's part of that eighth; the first ``[genesis]`` line on
stderr names the size used.

Memory
~~~~~~

The tool holds at most ``--memory-budget`` bytes (default: a third of the
memory available to the process, so a cgroup limit is respected) however large
a run is. An eighth of it goes to the gzip writer, and the rest is one pool
for the run groups that are processed at the same time. The largest groups
start first. A group takes its part of the pool by trace bytes, and at least
an even split between the groups at work (64 MB or more), and holds it until
its runs are written; a group waits while the pool is short. Within a run, the
decoded calls stay in memory up to their
share; the rest go to a temporary file, are sorted there in runs of the
share's size, and are merged back in order. Each worker writes a run as soon
as it is done, and the members are compressed and written in parallel, so no
run waits for another. The output holds the same lines whatever the budget
and the thread count, but not in the same order: runs, and the members of one
run, appear in the order they finish.

The ``counter`` records come first, and the ``run`` and ``func`` records
follow in members of their own at the end of the file. A ``func`` record is
followed by about two hundred ``counter`` records, so with both kinds in one
member every chunk of the index holds a few ``func`` records and a query on
them reads most chunks. Kept apart, a query on ``func`` or ``run`` records
reads the few chunks at the end: on a 197 MB sweep part, 12 chunks of 596
instead of 206 of 732. The ``run`` and ``func`` records go to
``<output>.head`` while the runs are written, and are appended to the output
when it closes.

The temporary file goes to ``DFTRACER_UTILS_SPILL_DIR`` when it is set. When it
is not, the tool picks the writable node-local disk mount with the most free
space, never a RAM-backed or network one. When no mount root is writable by
you, it uses a per-user directory under ``/var/tmp`` or ``~/.cache`` on a local
disk, then the system temp directory or ``~/.cache/dftracer-utils/spill`` if
that is not RAM-backed (a network file system qualifies). It never spills to a
RAM-backed directory, where the spilled data would stay in memory: when nothing
else is writable, the first spill fails with an error that names
``DFTRACER_UTILS_SPILL_DIR``. A node without a local disk (most diskless HPC
compute nodes) needs the variable set to a parallel file system. The file is
removed as soon as it is created, so nothing is left behind.

.. code-block:: bash

   # Keep the tool under 64 GB; name the spill directory only to override it
   dftracer_genesis_gen_dist ./laghos -o genesis.pfw.gz --memory-budget 64GB

A spill directory that sits in memory counts against the memory limit, so
only set ``DFTRACER_UTILS_SPILL_DIR`` to a disk. A run's output lines are made
1 MiB at a time from its records as they are written, and the group keeps its
share until then. Memory beyond the budget is still used by the process itself (threads,
libraries, I/O buffers, about 50 MB) and by the decoded size of one gzip
member per open reader: a
trace written as a single huge gzip member is decoded whole, so split it first
with ``dftracer_split``.

The counter series and path records of a run grow with its call paths times
its counters, not with its trace bytes, so a small trace can need a large
share. Once a group's calls are sorted, its walk also takes the readers' and
the calls' parts of its share, which it no longer needs. A group whose counter
series, path records or counter statistics still exceed its share borrows
memory of the pool that no group holds, 256 MB at a time, and returns it with
its share. Only when the pool has none free does it run again with four times
the share, up to the whole pool, and stderr names the group and the new share.
Only a run that does not fit the whole pool is skipped, with a reason that
names the share; raise ``--memory-budget`` for it.

A run that cannot be used is skipped with one line on stderr naming the run
directory, the file and the reason, for example a truncated ``.pfw.gz``, a
process without its ``start`` or ``end`` event, missing time slices or a
child call that ends after its parent. The other runs are still written. The
exit status is 0 when runs are skipped and 1 only when the output cannot be
written.

Progress
~~~~~~~~

The tool prints one line to stderr at most every 10 s:

.. code-block:: text

   [genesis 31m54s] groups 0/3 | phase wait 2, read 0, sort 0, walk 1, write 0 | read 68.6/68.8 GB 0.0 B/s | sort 10.3/10.3 G calls 0.0/s | walk 3.1/10.3 G calls 7.6 M/s | mem 43.8/79.5 GB peak | ETA 16m02s

``phase`` counts the groups that wait for their memory share, read their
files, sort their calls, walk them or write their runs. ``read`` is the trace
bytes read, ``sort`` and ``walk`` are the calls of the groups read so far with
their rates since the previous line, ``mem`` is the peak RSS against the
budget, and the ETA adds the reading left at the rate since the start to the
sorting and walking left at their current rates. At most every 60 s each group
that has run for a minute or more gets a line of its own:

.. code-block:: text

   [genesis 31m04s]   ppn_16  walk 2.8/10.3 G calls  6m13s in walk, 31m04s in group

A group's calls are sorted and walked in parallel: the processes of the group
go into up to 64 parts, process ``p`` into part ``p`` modulo the parts, each
part is walked into partial records of its own, and the partials are merged in
part order. The split depends on the group alone, so the output is the same
for any thread count. The partials count against the group's share, so a group
of many processes needs a larger share than a walk of one part would.

Split a sweep across nodes
~~~~~~~~~~~~~~~~~~~~~~~~~~

``--shard i/N`` processes part ``i`` of ``N`` of the run groups (``0 <= i <
N``). Every process walks the same roots and splits the groups the same way:
largest first, each to the part with the fewest trace bytes so far, so the
parts hold about the same bytes. Problems found before reading traces are
reported by part 0 only. Each part is a complete ``.pfw.gz`` file, and the
parts concatenate into one file, because a gzip file can hold any number of
members:

.. code-block:: bash

   # One part per node, then join the parts
   for i in 0 1 2 3; do
     flux submit -N1 -x dftracer_genesis_gen_dist ./sweep -o part-$i.pfw.gz --shard $i/4
   done
   flux queue drain
   cat part-0.pfw.gz part-1.pfw.gz part-2.pfw.gz part-3.pfw.gz > genesis.pfw.gz

A build with ``DFTRACER_UTILS_ENABLE_MPI=ON`` splits the groups between its
ranks without ``--shard``, which it refuses with more than one rank. Rank
``r`` of ``N`` takes part ``r``, writes it to ``<output>.<r>.part``, and the
ranks then copy their parts into ``-o`` at their offsets at the same time and
remove them; rank 0 prints the totals. Ranks on one node share its cores and,
when ``--memory-budget`` is not set, its automatic budget. The exit status is
1 when any rank cannot write its part or its copy.

.. code-block:: bash

   flux run -N8 --tasks-per-node=1 -x dftracer_genesis_gen_dist ./sweep -o genesis.pfw.gz

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

The same reads in pandas style: ``filter`` selects records in the scan, and
the frame it returns takes pandas spellings such as masks, ``merge`` and
``groupby``.

.. code-block:: python

   from dftracer.utils import col

   # Every call path, with the settings of its run.
   func = tv.filter(col("gtype") == "func").collect().merge(runs, on="run")
   slow = func[func["dur.p50"] > 1000]

   # The counters of one run, one column per counter.
   ctr = tv.filter((col("gtype") == "counter") & (col("run") == runs["run"][0])).collect()
   per_path = ctr.pivot_table(index="path", columns="metric", values="v.sum", aggfunc="sum")

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
serialized form. In a query, ``quantile(merge(dur.sketch), 0.5)`` merges the
sketches of a group and reads the quantile of their union, such as the median
call of ``main`` over every run of one setting. In C++, ``BasicDDSketch``
deserializes one.

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
