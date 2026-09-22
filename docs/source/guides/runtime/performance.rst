:description: The runtime tuning surface in one place: worker thread count and the knobs to reach for when a scan, aggregation, or plugin run is slow.

Tune runtime performance
=========================

.. admonition:: Goal
   :class: goal

   Know which knob to reach for when a scan, aggregation, or plugin run is
   slower or heavier than it should be. This page collects the tuning surface in
   one place; it does not re-explain the runtime model (see
   :doc:`../../concepts/async-runtime`) or memory budgeting (see
   :doc:`memory-budget`, not duplicated here).

Worker thread count
--------------------

The number of worker threads the runtime schedules coroutines on is the first
knob. More threads help a workload that is CPU-bound across many independent
chunks; too many on a shared or oversubscribed node hurts more than it helps.

.. tab-set::

   .. tab-item:: C++

      ``Runtime`` (``dftracer/utils/core/runtime.h``, namespace
      ``dftracer::utils``) takes a thread count directly, or a full
      ``ExecutorConfig`` for finer control:

      .. code-block:: cpp

         #include <dftracer/utils/core/runtime.h>
         using namespace dftracer::utils;

         Runtime rt(8);   // 8 worker threads; 0 = hardware_concurrency

         ExecutorConfig config;
         config.num_threads = 8;      // running cap; 0 = hardware_concurrency
         config.io_pool_size = 4;     // I/O thread pool; 0 = hardware_concurrency
         config.min_workers = 2;      // elastic floor: start at 2, grow to
                                       // num_threads under backlog, idle-retire
                                       // back down (0 = eager, spawn all up front)
         Runtime elastic_rt(config);

      ``DFTRACER_UTILS_THREADS`` overrides any requested count at process
      start (set it to ``1`` for a single-threaded async loop while
      debugging) - it takes precedence over both the constructor argument and
      ``ExecutorConfig::num_threads``.

   .. tab-item:: Python

      ``Runtime`` (``dftracer.utils.Runtime``) wraps the same C++ executor and
      adds a separate pool for plain Python callables:

      .. code-block:: python

         from dftracer.utils import Runtime, TraceViewer

         rt = Runtime(threads=8, io_threads=8, python_threads=4)
         tv = TraceViewer("./traces", runtime=rt)  # view execution uses rt's pool

      - ``threads``: C++ executor worker threads (0 = hardware_concurrency).
      - ``io_threads``: C++ I/O thread pool (0 = hardware_concurrency).
      - ``python_threads``: a ``ThreadPoolExecutor`` size for
        ``rt.submit(python_callable)`` (0 = ``min(32, threads)``); it does not
        affect C++-side scan/aggregation work.

      Without an explicit ``runtime=``, a viewer runs on a process-wide
      default runtime. Pass a ``Runtime`` explicitly when you need a
      non-default thread count or want several viewers to share one pool.

Data-parallel loops: parallel_for / parallel_reduce
---------------------------------------------------------

``Runtime::parallel_for`` and ``Runtime::parallel_reduce`` (same header) are
C++-only fork-join helpers over an index range, used internally by the
columnar and scan code and available to any C++ caller. There is no Python
binding; from Python, parallelism comes from the view/DataFrame APIs that use
these internally, not from calling them directly.

.. code-block:: cpp

   Runtime& rt = dftracer::utils::default_runtime();

   // Split [0, n) into grain-sized chunks, run body on the pool, block until done.
   rt.parallel_for(n, /*grain=*/4096, [&](std::int64_t begin, std::int64_t end) {
       for (std::int64_t i = begin; i < end; ++i) process(i);
   });

   // Same split, but each chunk maps to a T and partials combine (associative).
   std::int64_t total = rt.parallel_reduce<std::int64_t>(
       n, /*grain=*/4096, /*identity=*/0,
       [&](std::int64_t begin, std::int64_t end) { return partial_sum(begin, end); },
       [](std::int64_t a, std::int64_t b) { return a + b; });

Both pick ``min(chunks, threads())`` workers and drain an atomic chunk counter
(dynamic, load-balanced scheduling), run inline serial for a single chunk, and
detect nesting - a call already inside a ``parallel_for``/``parallel_reduce``
body runs serial rather than forking a nested fork-join. ``grain`` is the
per-chunk unit size: too small and the atomic-counter overhead dominates, too
large and load balancing degrades to one chunk per worker regardless of skew.

What the DataFrame engine parallelizes
--------------------------------------

The dataframe library is a leaf with no runtime dependency, so its kernels
fan out through a seam (``dataframe/parallel.h``) that a host installs
once: ``install_runtime_parallel_backend()`` in C++, done for you by the
Python extension at import. Until a backend is installed the engine runs
serial; with one, these kernels split their input over the pool and merge
the partials: group-by (a partial state per thread, merged many-way, or
for a string key with many groups a scatter into per-thread partitions),
expression evaluation, arithmetic (``//``, ``%`` and ``**`` included,
a scalar operand as a constant, not a broadcast column), comparisons
and filters, select / take, the reductions and field statistics, the
string kernels and predicates, casts,
sort (a sample sort: sampled splitters, one scatter, a SIMD sort per
bucket), dictionary encoding, dedup (``unique`` / ``is_duplicated``), the
hash join's probe and its index lists, the gathers behind filter / take /
sort, the prefix scans (``cumsum`` and the rest) as a two-pass parallel
scan, the rolling windows (each chunk warms its window up from the rows
before it). Work below the grain stays serial, so a small frame never
pays the fan-out. The temporal kernels, the moment statistics (``skew``
/ ``kurt``) and the logical ops run serial today.

Against pandas, polars and DuckDB
---------------------------------

``benchmarks/dataframe_vs_pandas_polars.py`` runs the same ops over the
same Arrow tables in all four engines, eagerly and as a plan on our
``LazyFrame``; the eager column is spelled exactly as the pandas one
(``df[df["v"] > 0.5]``, ``groupby("k")["v"].agg(["sum", "mean"])``,
``groupby(df["ts"] // 1_000_000)``, ``nlargest``, ``.str.contains``),
so the same code runs on both: a generic table and a trace-shaped one (``ts``, ``dur``,
``pid``, ``tid``, ``name``, ``cat``) with the analyses a trace tool runs.
After the timing it checks every engine's result against ours (the row
count, then each column's sorted values) and reports how many cores each
engine kept busy; ``--memory`` runs every (op, engine) in its own process
and reports the resident set's rise during the op. 10M rows, an Apple
M4 Pro (10 performance and 4 efficiency cores), best of 5, September
2026, all times in milliseconds; a ratio above 1 means we are faster:

.. list-table::
   :header-rows: 1
   :widths: 30 7 7 7 7 7 12 12

   * - op
     - ours
     - lazy
     - pandas
     - polars
     - duckdb
     - polars / ours
     - duckdb / ours
   * - filter ``v > 0.5`` (4 columns)
     - 8.5
     - 8.8
     - 38
     - 9.5
     - 97
     - 1.1x
     - 11x
   * - group_by k: sum v, mean v
     - 8.8
     - 9.1
     - 94
     - 26
     - 10
     - 3.0x
     - 1.1x
   * - sort by v
     - 106
     - 106
     - 1301
     - 116
     - 697
     - 1.1x
     - 6.6x
   * - join on k (inner, 10k build rows)
     - 16
     - 17
     - 181
     - 21
     - 352
     - 1.3x
     - 22x
   * - rolling(100).mean
     - 3.4
     - 3.5
     - 43
     - 86
     - 951
     - 25x
     - 281x
   * - string contains
     - 8.6
     - 10
     - 552
     - 85
     - 80
     - 9.8x
     - 9.2x
   * - cumsum
     - 1.3
     - 1.4
     - 15
     - 33
     - 194
     - 25x
     - 144x
   * - filter then group_by
     - 13
     - 9.0
     - 68
     - 18
     - 13
     - 1.3x
     - 1.0x
   * - trace: 1s time buckets, count + sum dur
     - 11
     - 8.0
     - 61
     - 26
     - 11
     - 2.4x
     - 1.0x
   * - trace: slowest 100 calls
     - 5.4
     - 4.9
     - 120
     - 96
     - 9.5
     - 18x
     - 1.8x
   * - trace: per (pid, name) mean / max dur
     - 18
     - 18
     - 284
     - 57
     - 19
     - 3.2x
     - 1.1x
   * - trace: POSIX calls over 100us, count per name
     - 12
     - 9.0
     - 237
     - 8.4
     - 11
     - 0.7x
     - 0.9x
   * - trace: per-pid busy time (sum dur)
     - 4.1
     - 3.3
     - 32
     - 3.1
     - 5.7
     - 0.8x
     - 1.4x
   * - trace: gaps between calls (ts diff), p99
     - 11
     - 14
     - 72
     - 34
     - 79
     - 3.1x
     - 7.2x

Faster than pandas on every row, polars on every row but two (a filtered
count per name, where the eager form gathers six columns, and a 64-group
sum, a millisecond behind at the noise floor), and DuckDB on every row
but the first of those, where the two are within a millisecond; the plan
form is ahead of polars there too. A plan runs at the eager speed, or ahead of it
where projection pushdown reads fewer columns; ``lazy`` p99 is the
streaming sketch quantile, exact to about 1e-3. Every engine's result
matches ours on every row. DuckDB's numbers are for a result
materialized to Arrow, which is what the other three return; its lazy
reader alone measures nothing.

Memory, the same run under ``--memory`` (the rise of the process's
resident set during the op, MB): ours is the lowest or within a few MB
of it on every row but three. A sort takes 870 against 400 for pandas and
polars: the argsort packs (key, index) into 16 bytes a row and scatters
into a second array (320 MB of scratch at 10M rows), and macOS's
allocator keeps freed blocks resident rather than reusing them for the
output columns; mapping the scratch from the kernel instead brought the
figure down but cost 40% of the sort's speed in page faults, so the
allocator stays. A filter keeps 274 against 177 for pandas, whose
filtered string column is 8-byte pointers to shared objects where ours
is the bytes. The partitioned group-by on (process, name) holds 60
against DuckDB's 52 (pandas 348, polars 554): a page per partition per
thread, the few full pages waiting on a queue, and the partition states.
Elsewhere: a group-by on 10k keys holds 35 (pandas 185,
polars 67); a rolling mean writes its output in place, 79 (pandas 239,
DuckDB 360); the slowest-100 keeps one run of candidates per thread, 58
(polars 393, pandas 620); a quantile reads its column in place and
copies one bucket, 99 for the p99 of a diff (polars 157, DuckDB 289).

The wins that got here, in the order they mattered: the expression
evaluator's chunk concat copied bit by bit and twice; the group-by kept a
heap vector per hash bucket and made a C ABI call per cell (now a plain
loop over batches of rows with a direct table for up to three dense
integer keys, a flat word table for string keys, one partial per thread
merged many-way and put back into first-seen order); the join
built a string per row, probed serially and grew its index lists by
push_back; the string predicates and the comparison kernel ran on one
core; the sort's last merge levels were one merge per thread pair (now a
sample sort, no merge); the gathers scanned their index list once per
column; a tumbling time window built a pair list and gathered every
column; a quantile sorted everything (now one bucket, read in place); a
plan over a resident frame streamed any op without an eager form
through a spool in 64k-row morsels and, past the auto budget (1 GB
assumed where the machine's free memory was unknown, now read on macOS
too), to disk (300 ms and 850 MB for a lazy cumsum, now 3 ms: every op
runs whole-column over the frame); a sketch quantile bin held 16 bits,
so a bucket past 65535 values returned -inf; a rolling window ran on one
core and copied its input and output twice; a top-k packed every row
before its partial sort; the group-by resolved a row's group with one
dependent load after another (a batch of rows now hashes first, then
reads its slots with the ones ahead prefetched, then its key words and
stats, then adds); a light group-by (count, sum, mean, min, max) kept an
88-byte statistic per field where 32 hold it; a count-only group-by
prefetched an empty statistics array, address zero, and paid a page
walk per row (four times slower than the same group-by with a sum);
``//``, ``%`` and ``**`` ran on one core and broadcast a scalar operand
into a column first (``ts // 1_000_000`` 8.6 to 1.1 ms); the pandas
``groupby(k)["v"].agg([...])`` went through the expression group-by
where the plain one serves (2.5 ms); a group-by on a string key
with many groups now scatters each row (row and hash in one word, key
words, values: 48 bytes for (pid, name, dur)) into a 32 KB page per
partition, four partitions per thread by hash; a full page goes on the
partition's lock-free queue and the thread that filled it, or whichever
holds the partition, folds it into the partition's state right away, so
packing and aggregating overlap with no barrier and the pages in flight
stay few. Each partition's table fits the first-level cache, where the
per-thread tables of the chunked path (about 2 MB each) contended for
the shared cache. Re-run the script on your machine and data shape; the
numbers move with both.

Where the copies are
--------------------

The engine is zero-copy where a copy would buy nothing: a Series filter
or slice returns a SELECTION view over the base column; a repeated
string column stays a DICTIONARY; ``share()`` is a refcount; ``to_arrow()``
and ``to_pandas(arrow=True)`` hand the engine's buffers to Arrow without
copying, ``to_polars()`` shares the numeric columns. Two places copy on
purpose: ``to_pandas()`` builds NumPy arrays, and a kernel over a view
materializes the view once before it runs (every kernel is written against
flat buffers; see :doc:`../../concepts/dataframe-model`). A frame's
``filter`` / ``sort_by`` / ``take`` gather every column flat, so a frame
filtered once and read many times pays the gather once, and a gather that
is the identity shares the column instead.

Where the memory budget fits in
-------------------------------------

Thread count controls how much CPU-parallel work runs at once; it does not
bound how much memory that work holds. A wide thread count over a large
group-by can build many large in-memory group maps in parallel and exhaust
memory faster than a narrow one would. See :doc:`memory-budget` for
``View::memory_budget`` / ``auto_spill`` (and the Python equivalents), which
cap and spill that side independently of the thread count set here.

See also
--------

- :doc:`../../concepts/async-runtime` for the coroutine/executor model these
  knobs configure.
- :doc:`memory-budget` for bounding memory instead of CPU parallelism.
- :doc:`task-graphs` for building an explicit DAG on top of this runtime.
- :doc:`../../concepts/coroutine-caveats` for rules to follow before sharing
  state across a parallel body.
