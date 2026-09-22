:description: Reference for the dftracer_* command-line tools and their shared pipeline, indexing, and query flags for working with trace files.

Command-Line Tools
==================

DFTracer Utils provides several command-line utilities for working with DFTracer trace files and compressed archives.

.. _cli-shared-flags:

Shared CLI Flags
----------------

Most tools wire in a common set of argument schemas defined in
``src/dftracer/utils/binaries/common_cli.h``. The flags below have identical
semantics across every binary that exposes the relevant schema and are not
repeated in each tool's section.

.. note::

   **Humanized sizes and durations.** Every ``<bytes>`` flag also accepts a
   unit suffix (``64MB``, ``1.5GiB``, ``512KB``, ``8kb``); byte units are
   1024-based, so ``KB`` == ``KiB``, ``B`` is bytes and ``b`` is bits (divided
   by 8). Every ``<s>`` flag also accepts a duration suffix (``30s``, ``5m``,
   ``1.5h``, ``500ms``, ``200us``). A **bare number keeps the flag's native
   unit**: a ``<bytes>`` flag reads it as bytes, an MB-scaled flag (e.g.
   ``--chunk-size <MB>``) reads it as MB, and a ``<s>`` flag reads it as
   seconds. The same coercion is available from the Python API, whose
   byte/duration arguments accept either a number or a unit string.

**Pipeline** (``PipelineArgs``)

- ``--executor-threads <count>`` - Number of worker threads for parallel
  processing (default: number of CPU cores)
- ``--io-threads <count>`` - Number of I/O threads (default: number of CPU
  cores)
- ``--time-profiling`` - Print stage timing breakdown to stderr
- ``--eager-thread-pools`` - Start the full worker pool up front instead of
  growing it on demand (lower first-batch latency, holds all threads)

**Indexing** (``IndexingArgs``)

- ``--index-dir <path>`` - Directory for ``.dftindex`` stores
- ``--checkpoint-size <bytes>`` - Checkpoint size for gzip indexing in bytes
  (default: 33554432 B / 32 MB). This is also the gzip member size: the
  checkpoint is the pruning/decode unit, so trace-writing tools (``split``)
  size their output gzip members to match. See :ref:`multi-member-gzip`.
- ``-f, --force`` - Force index recreation

**Query** (``QueryArgs``)

- ``--query <query>`` - Query DSL filter
  (e.g., ``'cat == "POSIX" and dur > 1000'``)

**Watchdog** (``WatchdogArgs``)

- ``--disable-watchdog`` - Disable watchdog for hang detection
- ``--watchdog-global-timeout <s>`` - Watchdog global timeout for pipeline
  execution in seconds (0 = no timeout, default: 0)
- ``--watchdog-task-timeout <s>`` - Watchdog default task timeout in seconds
  (0 = no timeout, default: 0)
- ``--watchdog-interval <s>`` - Watchdog check interval in seconds
  (default: 1)
- ``--watchdog-warning-threshold <s>`` - Watchdog long-running task warning
  threshold in seconds (default: 300)
- ``--watchdog-idle-timeout <s>`` - Watchdog idle timeout in seconds
  (0 = use default, default: 300)
- ``--watchdog-deadlock-timeout <s>`` - Watchdog deadlock timeout in seconds
  (0 = use default, default: 600)

**Inputs** (``DirectoryArgs`` / ``FilesArgs``)

- ``-d, --directory <path>`` - Directory containing trace files
- ``--files <files...>`` - Trace files (``.pfw.gz``)

**Logging**

- ``--log-level <level>`` - Logging verbosity: ``trace``, ``debug``, ``info``
  (default), ``warn``, ``error``, or ``off``. Available on every tool; overrides
  the ``DFTRACER_UTILS_LOG_LEVEL`` environment variable (see
  :doc:`getting-started/installation`).

.. _multi-member-gzip:

Multi-member gzip output
------------------------

The trace-writing tools (``split``) emit **multi-member gzip**: each
compressed ``.pfw.gz`` file is a sequence of independent gzip members rather
than one monolithic stream.

**Why it matters.** A single-member gzip stream can only be inflated serially,
so indexing and reading a large file is limited to one core per file.
Multi-member output lets readers and the indexer divide a single file into
per-member work and inflate/parse it in parallel (see the ``gzip_member_scanner``
and the parallel index build). This is what keeps large traces fast without
first building an index. The native DFTracer runtime writes multi-member gzip;
tools that re-chunk traces should preserve that shape.

**Sizing.** The gzip member size is the same knob as the index checkpoint
size, ``--checkpoint-size`` (uncompressed bytes), because the gzip member is
the pruning/checkpoint unit. This is intentionally a different unit from the
tools' ``--chunk-size`` (which controls the **compressed** on-disk file size):

- ``--chunk-size`` (compressed) - how big each output file is on disk.
- ``--checkpoint-size`` (uncompressed) - the intra-file parallelism granularity.

Because a chunk is sized in compressed bytes (roughly 10x smaller than
uncompressed), a chunk comfortably holds many members. For example a 32 MB
compressed chunk is ~300 MB uncompressed, so a 4 MB ``--checkpoint-size``
yields roughly 75 members (300 / 4). Smaller members give more parallelism at
a small compression-ratio cost (each member resets the deflate dictionary);
larger members compress slightly better with coarser parallelism.

dftracer_info
-------------

**Description:** Display metadata and index information for DFTracer compressed files

**Usage:**

.. code-block:: bash

   dftracer_info [OPTIONS]

**Options:**

- ``--files <files...>`` - Compressed files to inspect (GZIP, TAR.GZ)
- ``-d, --directory <path>`` - Directory containing files to inspect
- ``--query <type>`` - Query type: ``summary`` (aggregate all files, default) or ``detailed`` (per-file output including detailed statistics)
- ``-f, --force-rebuild`` - Force rebuild index files
- ``--checkpoint-size <bytes>`` - Checkpoint size for indexing in bytes (default: 33554432 B / 32 MB)
- ``--index-dir <path>`` - Directory to store index files (default: system temp directory)
- ``--executor-threads <count>`` - Number of worker threads for parallel processing (default: number of CPU cores)

**Example:**

.. code-block:: bash

   # Show info for files in a directory
   dftracer_info -d ./logs

   # Show detailed per-file info
   dftracer_info --files trace1.pfw.gz trace2.pfw.gz --query detailed

   # Per-file detailed output
   dftracer_info -d ./traces --query detailed

   # Analyze with 4 threads
   dftracer_info --executor-threads 4 -d ./traces

dftracer_split
--------------

**Description:** Split DFTracer traces into equal-sized chunks using pipeline processing

**Usage:**

.. code-block:: bash

   dftracer_split [OPTIONS]

**Options:**

- ``-n, --app-name <name>`` - Application name for output files (default: app)
- ``-d, --directory <path>`` - Input directory containing .pfw.gz files (default: .)
- ``-o, --output <dir>`` - Output directory for split files (default: ./split)
- ``-s, --chunk-size <MB>`` - Output file size in MB, approximate **compressed** on-disk size (default: 4)
- ``-f, --force`` - Override existing files and force index recreation
- ``-c, --compress`` - Compress output files with gzip (a value-less flag;
  output is gzip-compressed by default)
- ``--checkpoint-size <bytes>`` - Checkpoint size for indexing in bytes; also
  the gzip member size (default: 33554432 B / 32 MB)
- ``--executor-threads <count>`` - Number of worker threads for parallel processing (default: number of CPU cores)
- ``--index-dir <path>`` - Directory to store index files (default: system temp directory)
- ``--verify`` - Verify output chunks match input by comparing event IDs

.. note::

   ``--chunk-size`` is **compressed** (the resulting on-disk file size), while
   ``--checkpoint-size`` is **uncompressed** (the parallel-work / gzip member
   granularity). They are orthogonal: a 32 MB compressed chunk holds ~300 MB
   uncompressed, so at an 8 MB checkpoint size it contains ~37 members. See
   :ref:`multi-member-gzip`.

**Example:**

.. code-block:: bash

   # Split files into 4MB chunks
   dftracer_split -d ./logs -o ./split_output

   # Split with 10MB chunks and custom app name
   dftracer_split -d ./traces -s 10 -n myapp -o ./chunks

   # Larger chunks with a fine-grained 4 MB checkpoint size for more read parallelism
   dftracer_split -d ./traces -s 256 --checkpoint-size 4194304 -o ./chunks

   # Verify that split output matches the input event IDs
   dftracer_split -d ./data --verify -o ./output

dftracer_event_count
--------------------

**Description:** Count valid events in DFTracer .pfw.gz files using pipeline processing

**Usage:**

.. code-block:: bash

   dftracer_event_count [OPTIONS]

**Options:**

- ``-d, --directory <path>`` - Directory containing .pfw.gz files (default: .)
- ``-f, --force`` - Force index recreation
- ``--checkpoint-size <bytes>`` - Checkpoint size for indexing in bytes (default: 33554432 B / 32 MB)
- ``--executor-threads <count>`` - Number of worker threads for parallel processing (default: number of CPU cores)
- ``--index-dir <path>`` - Directory to store index files (default: system temp directory)

**Example:**

.. code-block:: bash

   # Count events in current directory
   dftracer_event_count

   # Count events in specific directory with 8 threads
   dftracer_event_count -d ./traces --executor-threads 8

   # Force index rebuild
   dftracer_event_count -d ./logs -f

dftracer_validate
-----------------

**Description:** Validate DFTracer ``.pfw.gz`` trace files by
confirming every non-wrapper line is valid JSON. A fast, parallel C++
equivalent of the ``dftracer_validate`` shell script.

**Usage:**

.. code-block:: bash

   dftracer_validate [OPTIONS]

**Options:**

- ``-d, --directory <path>`` - Directory scanned recursively (and in parallel)
  for ``.pfw.gz`` files
- ``--files <files...>`` - Explicit trace files to validate
- ``--executor-threads <count>`` - Worker threads for parallel validation
  (default: number of CPU cores)

Provide ``-d`` and/or ``--files`` (at least one is required). Files are
validated concurrently; a line counts as invalid if it is not well-formed JSON
(the wrapper ``[`` / ``]`` lines and blanks are skipped). The command exits
non-zero if any file fails validation or no files are found. See also the
shared :ref:`cli-shared-flags` (e.g. ``--log-level``).

**Example:**

.. code-block:: bash

   # Validate every trace under a directory (recursive, parallel)
   dftracer_validate -d ./traces

   # Validate specific files
   dftracer_validate --files run0.pfw.gz run1.pfw.gz

   # Show per-file detail
   dftracer_validate -d ./traces --log-level debug

dftracer_pgzip
--------------

**Description:** Parallel gzip compression for DFTracer .pfw files

**Usage:**

.. code-block:: bash

    dftracer_pgzip [OPTIONS]

**Options:**

- ``-d, --directory <path>`` - Directory containing .pfw files (default: .)
- ``-l, --compression-level <0-12>`` - Compression level (default: 6)
- ``--chunk-size <bytes>`` - Chunk size in bytes for parallel compression (default: 4194304 B / 4 MB)
- ``--executor-threads <count>`` - Number of worker threads for parallel processing (default: number of CPU cores)

**Example:**

.. code-block:: bash

    # Compress all .pfw files in current directory
    dftracer_pgzip

    # Compress files in a specific directory with debug logging
    dftracer_pgzip -d ./logs --log-level debug

    # Maximum compression with a larger parallel chunk size
    dftracer_pgzip -d ./logs -l 12 --chunk-size 8388608

    # Compress with 16 threads
    dftracer_pgzip -d ./traces --executor-threads 16

dftracer_server
---------------

**Description:** HTTP server for querying and streaming DFTracer trace data via REST API

**Usage:**

.. code-block:: bash

     dftracer_server [OPTIONS] --directory <path>

**Options:**

- ``-b, --bind <address>`` - Bind address (default: 127.0.0.1; use 0.0.0.0 to expose on all interfaces)
- ``-p, --port <number>`` - Listen port (default: 8080)
- ``-d, --directory <path>`` - Directory containing trace files [required]
- ``--index-dir <path>`` - Directory for root-local ``.dftindex`` stores (default: same as --directory)
- ``--token <token>`` - Optional access token; when set, every request must
  supply it via ``?token=`` or an ``Authorization: Bearer <token>`` header
- ``--checkpoint-size <bytes>`` - Decompression checkpoint interval in bytes
  for auto-indexing (default: 33554432 B / 32 MB). Smaller = finer zoom-in
  seeks, larger index
- ``--member-cache-size <bytes>`` - Bytes of decoded gzip members retained to
  share across concurrent queries (0 disables retention but still coalesces
  in-flight decodes; default: 1073741824 B / 1 GB)
- ``--timeout <s>`` - Auto-shutdown after this much uptime (0 disables,
  default). Useful to stop lingering processes when a client such as the
  VSCode extension closes without killing the server
- ``--executor-threads <count>`` - Number of worker threads (default: number of CPU cores)

**Example:**

.. code-block:: bash

     # Start server on default port 8080, bound to localhost
     dftracer_server -d ./traces

     # Expose on all interfaces on a custom port
     dftracer_server -b 0.0.0.0 -p 9000 -d ./traces

     # Start with custom index directory, an access token, and thread count
     dftracer_server -d ./traces --index-dir /var/cache/dftracer_indexes \
         --token secret123 --executor-threads 8

dftracer_stats
--------------

**Description:** Compute event statistics with bloom filter acceleration and detailed distribution analysis

**Usage:**

.. code-block:: bash

    dftracer_stats [OPTIONS]

**Options:**

- ``-d, --directory <path>`` - Directory containing .pfw.gz files (default: .)
- ``--files <files...>`` - Explicit list of trace files
- ``--index-dir <path>`` - Directory to store index files (default: system temp directory)
- ``--report <type>`` - Report type: summary, categories, names, pid_tids, time_range, duration, top-names, top-categories, detailed (default: summary)
- ``--top-n <count>`` - Number of results for top-N queries (0=show all, default: 0)
- ``--top-n-pid-tid <count>`` - Max PID:TID pairs to display (0=show all, default: 10)
- ``--query <query>`` - Query DSL filter (e.g., ``'cat == "POSIX" and dur > 1000'``)
- ``--group-by <dims...>`` - Group-by dimensions: name, cat, pid, tid, fhash, hhash, pid_tid (default: name for detailed)
- ``--filter-names <names...>`` - Filter by event names
- ``--filter-cats <cats...>`` - Filter by event categories
- ``--json`` - Output in JSON format
- ``--no-auto-index`` - Disable automatic bloom index building
- ``--checkpoint-size <bytes>`` - Checkpoint size for indexing in bytes (default: 33554432 B / 32 MB)
- ``--executor-threads <count>`` - Number of worker threads (default: number of CPU cores)

**Example:**

.. code-block:: bash

    # Summary statistics
    dftracer_stats -d ./traces

    # Top operations and categories
    dftracer_stats -d ./traces --report categories

    # Detailed duration distribution per operation
    dftracer_stats -d ./traces --report detailed --group-by name --top-n 20

    # Filter to POSIX operations only
    dftracer_stats -d ./traces --report duration --query 'cat == "POSIX"'

dftracer_view
-------------

**Description:** Extract filtered subsets of trace data using query-based filtering with chunk pruning

**Usage:**

.. code-block:: bash

    dftracer_view [OPTIONS]

**Options:**

- ``--files <files...>`` - Trace files to process (.pfw.gz)
- ``-d, --directory <path>`` - Directory containing trace files
- ``--preset <name>`` - Predefined view: io, compute, dlio
- ``--recipe <path>`` - Custom view JSON file path
- ``--save-recipe <path>`` - Save the constructed view to a JSON file
- ``--query <query>`` - Query DSL filter (e.g., ``'cat == "POSIX" and dur > 1000'``)
- ``--time-range <min,max>`` - Timestamp filter in microseconds (e.g., 1000000,2000000)
- ``--min-duration <us>`` - Minimum event duration in microseconds
- ``--max-duration <us>`` - Maximum event duration in microseconds
- ``-o, --output <path>`` - Output file path (default: stdout)
- ``--stream`` - Stream matching events to stdout as NDJSON
- ``--no-metadata`` - Exclude metadata events (ph=M) from output
- ``--index-dir <path>`` - Directory where ``.dftindex`` stores are created
- ``--no-auto-index`` - Disable automatic index building for files missing ``.dftindex``
- ``--checkpoint-size <bytes>`` - Checkpoint size for auto-indexing in bytes (default: 33554432 B / 32 MB)
- ``--executor-threads <count>`` - Number of worker threads (default: number of CPU cores)

**Aggregation:**

- ``--group-by <cols>`` - Aggregate: group by columns, comma-separated. Any
  field name works (top-level then args); named dims are ``name``, ``cat``,
  ``pid``, ``tid``, ``fhash``, ``hhash``, ``io_cat``, ``acc_pat``,
  ``file_path``, ``file_name``, ``host_name``, ``rank`` (pid resolved to MPI
  rank via ``PR`` metadata), or ``arg:KEY`` for an args
  field. Dotted/bracketed paths descend nested args (``args.meta.host``,
  ``args.tags[0]``) - see :doc:`guides/analysis/aggregation`
- ``--agg <reducers>`` - Aggregate: reducers, comma-separated (``count``,
  ``sum:FIELD``, ``min:FIELD``, ``max:FIELD``, ``mean:FIELD``, ``var:FIELD``,
  ``std:FIELD``, ``skew:FIELD``, ``kurt:FIELD``, ``pNN:FIELD`` e.g.
  ``p99:dur``, ``pct:FIELD:Q``, and the field-less occupancy reducers ``busy``,
  ``concurrency``, ``utilization``, ``active`` - see
  :doc:`guides/analysis/aggregation`)
- ``--time-bucket <us>`` - Aggregate into time buckets of N microseconds
- ``--occ-cell <us>`` - Occupancy cell size (busy quantum) for
  ``busy``/``concurrency``/``utilization``; finer resolves overlap on short
  events (honored with ``--time-range``, default 64 us)
- ``--counters`` - Emit the aggregation as ``ph=C`` counter events
- ``--phase <phase>`` - Select events by phase: ``events`` (ph=X),
  ``counters`` (ph=C), ``aggregated`` (rollup records), ``metadata`` (ph=M),
  ``any``
- ``--select <cols>`` - Project the result to these columns, comma-separated
- ``--limit <n>`` - Cap the output to N rows/events (0 = unlimited)
- ``--offset <n>`` - Skip the first N rows/events before applying ``--limit``
- ``--time-scale <ratio>`` - Scale timestamps/durations by this
  ns-per-unit ratio (0 = leave as stored)
- ``--memory-budget <bytes>`` - Spill aggregation to disk past N in-core
  bytes (0 = auto: ~1/3 of available memory)
- ``--no-spill`` - Keep aggregation fully in memory (disable the default spill)
- ``--agg-numeric-args`` - Aggregate every numeric ``args.*`` field automatically
- ``--collect-typed`` - Dump the aggregation index's regular/aggregated/counters families (one pass)
- ``--materialize`` - Persist this aggregation as a rollup for instant reuse (needs ``--group-by``/``--agg``)

**Merging:**

- ``--merge`` - Merge all inputs into one trace written to ``--output``
- ``--no-index`` - Do not build an index for the written trace (default: index)
- ``--verify`` - Re-scan the exported output and confirm the event count round-trips

**Call tree / flamegraph:**

- ``--call-tree`` - Emit the containment call tree as NDJSON
- ``--flamegraph`` - Emit the folded flamegraph as NDJSON. Distributes under
  ``mpirun``: each rank folds an arena partial, ranks all-gather, and rank 0
  merges.
- ``--ct-partition <keys>`` - Comma-separated lane keys for
  ``--call-tree``/``--flamegraph`` (default: ``pid,tid``)

**Example:**

.. code-block:: bash

    # Extract I/O operations
    dftracer_view --preset io -d ./traces -o io_events.pfw

    # Custom query: POSIX read/write operations
    dftracer_view -d ./traces --query 'cat == "POSIX" and name in ["read", "write"]' -o posix_rw.pfw

    # Time-filtered view with output streaming
    dftracer_view -d ./traces --time-range 1000000,5000000 --stream

dftracer_index
--------------

**Description:** Build per-chunk bloom filter indices for efficient chunk-skipping queries

**Usage:**

.. code-block:: bash

    dftracer_index [OPTIONS]

**Options:**

- ``-d, --directory <path>`` - Input directory containing .pfw.gz files (default: .)
- ``--dimensions <dims>`` - Comma-separated extra dimensions to index from args (e.g., args.level,args.mode)
- ``-f, --force`` - Force index recreation even if already built
- ``--checkpoint-size <bytes>`` - Checkpoint size for gzip indexing in bytes (default: 33554432 B / 32 MB)
- ``--executor-threads <count>`` - Number of worker threads for parallel processing (default: number of CPU cores)
- ``--index-dir <path>`` - Directory to store index files (default: same as data files)
- ``--expected-entries <count>`` - Expected entries per chunk for bloom filter sizing (default: 1024)
- ``--false-positive-rate <rate>`` - Bloom filter false positive rate (default: 0.01)
- ``--read-batch-size <MB>`` - Batch read size in MB for stream processing (default: 4)
- ``--rebuild-summaries`` - Rebuild ``ROOT_*`` aggregated summaries after ingest.
  Off by default; ``ROOT_*`` CFs are only consumed by summary tools such as
  ``dftracer_info``. Bloom-filter chunk-skipping queries do not require them.

This binary also accepts the shared :ref:`cli-shared-flags` (Pipeline,
Indexing).

**Example:**

.. code-block:: bash

    # Build bloom indices for all traces
    dftracer_index -d ./traces

    # Build with custom dimensions and force rebuild
    dftracer_index -d ./traces --dimensions "args.level,args.io.size" --force

    # Rebuild ROOT_* aggregated summaries after ingest
    dftracer_index -d ./traces --rebuild-summaries

dftracer_run
------------

**Description:** Load one or more compiled plugins (``.so``) and run them
over a directory or file list of DFTracer traces. Files are normalized to
multi-member gzip (so every checkpoint is a real gzip member) before the
plugins run, unless ``--no-auto-index`` is given.

**Usage:**

.. code-block:: bash

    dftracer_run [OPTIONS] --plugin <path.so> [--plugin <path.so> ...]

**Options:**

- ``-d, --directory <path>`` - Directory containing trace files
- ``--files <files...>`` - Trace files (.pfw.gz)
- ``--index-dir <path>`` - Directory where ``.dftindex`` stores are created
- ``--checkpoint-size <bytes>`` - Checkpoint size for gzip indexing in bytes (default: 33554432 B / 32 MB)
- ``--no-auto-index`` - Disable automatic index building for files missing ``.dftindex``
- ``--describe`` - Load ``--plugin`` libraries, print what each provides/consumes,
  and exit without scanning
- ``--plugin <path>`` - Path to a plugin shared object. Repeatable; each
  occurrence starts a new plugin block that the following ``--parg``/``--pconfig``
  apply to
- ``--parg <key=value>`` - Set a config key for the plugin block started by
  the preceding ``--plugin``. Repeatable
- ``--pconfig <path>`` - JSON config file merged into the plugin block
  started by the preceding ``--plugin``
- ``--shared-parg <key=value>`` - Set a config key applied to every plugin
  block. Repeatable
- ``--shared-pconfig <path>`` - JSON config file merged into every plugin
  block

These flags are extracted before argparse runs, so they can be interleaved
freely with the tool's other options. This binary also accepts the shared
:ref:`cli-shared-flags` (Pipeline and Watchdog schemas).

**Example:**

.. code-block:: bash

    # Run a single plugin over a directory of traces
    dftracer_run -d ./traces --plugin ./libmy_plugin.so

    # Pass per-plugin args and a shared config file to two plugins
    dftracer_run -d ./traces \
        --plugin ./libfoo.so --parg mode=fast \
        --plugin ./libbar.so --pconfig bar.json \
        --shared-pconfig shared.json

dftracer_gen_dlio_config
------------------------

**Description:** Generate a DLIO YAML configuration directly from a directory
of raw DFTracer traces. The tool indexes the inputs, aggregates them into the
internal ``AGGREGATION`` column family (DDSketch forced on), fits per-component
distributions, refines ``max_bound`` against an internal barrier simulator, and
emits a DLIO ``train.computation_time`` + ``reader.preprocess_time`` block. No
separate aggregation step is required; the tool populates the ``AGGREGATION``
column family itself.

Default input event names: ``cat=dataloader`` with ``name=fetch.block`` /
``fetch.iter``, and ``cat=data`` with ``name=preprocess`` / ``item``. Traces
that label these phases differently can be remapped with ``--event-map`` (see
below). The tool exits non-zero with an explanatory message if no DLIO events
are present.

**Usage:**

.. code-block:: bash

    dftracer_gen_dlio_config [OPTIONS] -o <config.yaml>

**Options:**

- ``-d, --directory <path>`` - Input directory containing .pfw.gz traces (default: .)
- ``-o, --output <path>`` - Output path for the DLIO YAML config [required]
- ``--max-bound-percentile <pct>`` - Initial max_bound percentile, 0-100 (default: 95)
- ``--simulation-iterations <n>`` - Max simulator iterations for percentile refinement (default: 5)
- ``--target-e2e-error <frac>`` - Target relative E2E error to declare convergence (default: 0.05)
- ``--target-cdf-similarity <frac>`` - Target fetch_block CDF similarity (default: 0.90)
- ``--patience <n>`` - Early-stop after this many iterations without improvement (default: 10)
- ``--epsilon <step>`` - Base step size for percentile adjustment (default: 1.0)
- ``--momentum <m>`` - Momentum factor in [0, 1) (default: 0.9)
- ``--min-percentile <pct>`` - Floor on max_bound percentile during optimization (default: 50)
- ``--num-workers <n>`` - DataLoader worker count for the simulator (default: 8)
- ``--prefetch-factor <n>`` - DataLoader prefetch factor (default: 2)
- ``--seed <n>`` - Base seed for simulator and sampler (default: 42)
- ``--max-samples-per-entry <n>`` - Cap on synthesized samples per aggregation entry; 0 disables (default: 100)
- ``-t, --time-interval <ms>`` - Aggregation time interval in ms (default: 5000)
- ``--event-map <path>`` - YAML or JSON file remapping the ``(cat, name)`` of the ``fetch_block`` / ``fetch_iter`` / ``preprocess`` / ``item`` components (see below)
- ``--index-dir <path>`` - Directory for the shared index store (default: system temp dir)
- ``--checkpoint-size <bytes>`` - Checkpoint size for indexing in bytes (default: 33554432 B / 32 MB)
- ``--executor-threads <count>`` - Number of executor threads for parallel processing
- ``-f, --force`` - Force index recreation

**Distribution pool:** Each component is fit as the lowest-BIC choice among
{Normal, Lognormal, Gamma, Exponential, Weibull, Gaussian Mixture (K=2),
Gaussian Mixture (K=3)}. Mixture candidates are only considered when the
sample count is at least 20.

**Event map:** ``--event-map`` points at a YAML or JSON file (JSON is parsed as
a YAML subset, so either form works) whose top-level keys ``fetch_block``,
``fetch_iter``, ``preprocess`` and ``item`` each carry an optional ``cat`` and
``name``. Omitted keys and fields keep the defaults above, so only the events
that differ need an entry.

.. code-block:: yaml

    # event_map.yaml
    fetch_block:
      cat: io
      name: read
    preprocess:
      cat: cpu
      name: transform

**Example:**

.. code-block:: bash

    # Generate config from a directory of raw traces
    dftracer_gen_dlio_config -d ./traces -o dlio_config.yaml

    # Refine harder against the simulator with a tighter convergence target
    dftracer_gen_dlio_config -d ./traces -o dlio_config.yaml \
        --simulation-iterations 20 --target-e2e-error 0.02 --patience 5

    # Reuse a shared index directory across runs to skip re-indexing
    dftracer_gen_dlio_config -d ./traces -o dlio_config.yaml \
        --index-dir /var/cache/dftracer/idx

    # Remap non-default event names onto the DLIO components
    dftracer_gen_dlio_config -d ./traces -o dlio_config.yaml \
        --event-map event_map.yaml

**Output schema:**

.. code-block:: yaml

    train:
      computation_time:
        type: <normal|lognormal|gamma|exponential|weibull|mixture>
        # single distribution: per-family params (mean/stdev, mu/sigma,
        # shape/scale, rate)
        # mixture: n_components + components: [{weight, params: {type, ...}}]
        max_bound: <seconds>
    reader:
      preprocess_time:
        # same structure

**Comparing against an external generator:** ``scripts/compare_dlio_yamls.py``
diffs two DLIO YAMLs with a tolerance check on parameters and a two-sample
Kolmogorov-Smirnov check on samples drawn from each fit. Run via ``uv run
scripts/compare_dlio_yamls.py --python <a.yaml> --cpp <b.yaml>`` (the inline
PEP-723 metadata installs ``pyyaml`` and ``numpy`` automatically). Same model
family + small KS = the two YAMLs would produce indistinguishable DLIO sample
streams.

dftracer_replay
---------------

**Description:** Replay I/O operations from DFTracer trace files with timing and filtering support

**Usage:**

.. code-block:: bash

    dftracer_replay [OPTIONS] <inputs...>

**Options:**

- ``inputs`` - Trace files (.pfw.gz) or directories containing trace files [required]
- ``--no-timing`` - Ignore original timing and execute as fast as possible
- ``--dry-run`` - Parse and analyze traces without executing operations
- ``--dftracer-mode`` - Use DFTracer sleep-based replay (sleep for operation duration instead of doing actual I/O)
- ``--no-sleep`` - When used with --dftracer-mode, disable sleep calls for maximum speed
- ``-r, --recursive`` - Recursively search directories for trace files
- ``--use-call-tree`` - Build and use call tree structure for hierarchical replay
- ``--hierarchical-replay`` - Replay operations respecting parent-child call hierarchy (requires --use-call-tree)
- ``--respect-call-hierarchy`` - Replay child nodes immediately after parent (requires --use-call-tree and --hierarchical-replay)
- ``--filter-pid <pids>`` - Only replay events from specific PID(s) (comma-separated)
- ``--exclude-pid <pids>`` - Exclude events from specific PID(s) (comma-separated)
- ``--filter-tid <tids>`` - Only replay events from specific TID(s) (comma-separated)
- ``--exclude-tid <tids>`` - Exclude events from specific TID(s) (comma-separated)
- ``--filter-function <funcs>`` - Only replay specific function(s) (comma-separated, e.g., read,write,open)
- ``--exclude-function <funcs>`` - Exclude specific function(s) (comma-separated)
- ``--filter-category <cats>`` - Only replay specific category/categories (comma-separated, e.g., POSIX,storage)
- ``--exclude-category <cats>`` - Exclude specific category/categories (comma-separated)
- ``--start-timestamp <us>`` - Only replay events after this timestamp (microseconds)
- ``--end-timestamp <us>`` - Only replay events before this timestamp (microseconds)
- ``--min-size <bytes>`` - Only replay operations with size >= this value (bytes)
- ``--max-size <bytes>`` - Only replay operations with size <= this value (bytes)
- ``--sample-rate <rate>`` - Sample rate for replay (0.0-1.0, 1.0=all events, 0.1=10%)
- ``--sample-seed <seed>`` - Random seed for sampling (for reproducibility)
- ``--max-events <count>`` - Maximum number of events to replay (0=unlimited)
- ``--channel-capacity <n>`` - Bounded ``Channel<Trace>`` capacity between the
  read/parse producer and the dispatch consumer (default: 4096)

**Example:**

.. code-block:: bash

    # Replay with original timing
    dftracer_replay ./traces/rank_0.pfw.gz

    # Dry-run analysis of trace file with debug logging
    dftracer_replay ./traces/rank_0.pfw.gz --dry-run --log-level debug

    # Replay only POSIX read operations
    dftracer_replay -d ./traces -r --filter-category POSIX --filter-function read

For detailed usage, see :doc:`utilities/replay`.

dftracer_gen_fake_trace
-----------------------

**Description:** Generate realistic synthetic DFTracer traces for testing bloom filter indexing

**Usage:**

.. code-block:: bash

    dftracer_gen_fake_trace [OPTIONS] --output-dir <dir>

**Options:**

- ``-o, --output-dir <dir>`` - Output directory for trace files [required]
- ``-p, --num-processes <count>`` - Number of ranks (default: 8)
- ``-H, --num-hosts <count>`` - Number of hosts (default: 4)
- ``-e, --num-epochs <count>`` - Training epochs (default: 500)
- ``-s, --steps-per-epoch <count>`` - Steps per epoch (default: 1000)
- ``--checkpoint-every <n>`` - Checkpoint every N epochs (default: 5)
- ``--validation-every <n>`` - Validate every N epochs (default: 2)
- ``--num-train-files <count>`` - Training data shards (default: 8)
- ``--num-val-files <count>`` - Validation data shards (default: 2)
- ``--step-duration-ms <ms>`` - Base step duration in milliseconds (default: 100)
- ``--seed <seed>`` - Random seed for duration jitter (default: 42)
- ``--verify`` - After generation, build bloom indices and run queries to verify chunk-skipping works
- ``--checkpoint-size <bytes>`` - Gzip checkpoint size in bytes for indexing (default: 2 MB)

**Example:**

.. code-block:: bash

    # Generate synthetic traces for 4 ranks
    dftracer_gen_fake_trace -o ./traces -p 4

    # Generate with verification of bloom filters
    dftracer_gen_fake_trace -o ./traces -p 8 -H 2 --verify

    # Generate with custom training parameters
    dftracer_gen_fake_trace -o ./traces -e 100 -s 500 --checkpoint-every 10

dftracer_comparator
-------------------

**Description:** Compare DFTracer trace metrics between a baseline and a variant run. Produces a hierarchical tree table showing per-category and per-operation deltas with Cohen's d significance classification.

**Usage:**

.. code-block:: bash

    dftracer_comparator [OPTIONS]

**Options:**

- ``--baseline <path>`` - Baseline trace file or directory [required unless ``--config``]
- ``--variant <path>`` - Variant trace file or directory [required unless ``--config``]
- ``--config <path>`` - JSON config file for hierarchical comparison (replaces ``--baseline``/``--variant``)
- ``--preset <name>`` - Built-in comparison preset (see **Presets** below). Requires ``--baseline`` and ``--variant``
- ``--query <query>`` - Query DSL filter. With ``--preset``, ANDed into every top-level node query to narrow results without replacing the preset structure (default: ``'cat == "POSIX" OR cat == "STDIO"'`` for plain mode)
- ``--group-by <keys>`` - Comma-separated group keys (default: cat,name)
- ``--format <fmt>`` - Output format: ``table`` (default) or ``json``
- ``-t, --time-interval <ms>`` - Time interval in milliseconds for bucketing (default: 5000)
- ``--threshold <pct>`` - Hide changes below this percentage (default: 0.0)
- ``--no-color`` - Disable ANSI color output
- ``--compact`` - Collapse nodes where all metrics are negligible to a single ``(no change)`` line; useful for large presets where most categories are unchanged
- ``--executor-threads <count>`` - Number of parallel threads (default: auto)
- ``--baseline-index-dir <path>`` - Index directory for baseline (default: co-located with data)
- ``--variant-index-dir <path>`` - Index directory for variant (default: co-located with data)
- ``-f, --force`` - Force index rebuild
- ``--checkpoint-size <bytes>`` - Checkpoint size for indexing in bytes (default: 33554432 B / 32 MB)

**Example:**

.. code-block:: bash

    # Quick comparison of two trace files
    dftracer_comparator --baseline run_v1.pfw.gz --variant run_v2.pfw.gz

    # Compare directories with 1-second buckets
    dftracer_comparator --baseline ./traces_v1 --variant ./traces_v2 -t 1000

    # JSON output for programmatic consumption
    dftracer_comparator --baseline run_v1.pfw.gz --variant run_v2.pfw.gz --format json

    # Filter to specific operations
    dftracer_comparator --baseline a.pfw.gz --variant b.pfw.gz \
        --query 'cat == "POSIX" AND name == "write"'

    # DLIO preset - full hierarchical coverage of all DLIO annotation categories
    dftracer_comparator --preset dlio --baseline ./run_v1 --variant ./run_v2

    # DLIO preset narrowed to a single host, with compact output
    dftracer_comparator --preset dlio --baseline ./run_v1 --variant ./run_v2 \
        --query 'hhash == "abc123"' --compact

    # Hierarchical comparison via JSON config
    dftracer_comparator --config compare.json

**Output format:**

The table output starts with a one-line summary preamble:

.. code-block:: text

    8 nodes  2 regressions  1 improvement  4 no data

followed by the comparison tree. Each node renders its metrics under a ``SUMMARY``
sub-node. Nodes with no matching events show ``(no data)`` inline. With
``--compact``, nodes where every metric is negligible show ``(no change)`` inline
instead of expanding.

After the tree, a **Top regressions** footer lists up to 5 nodes with the largest
percentage regressions (significance >= ``MEDIUM``). The footer is suppressed when
there are no regressions.

**Output columns:**

- **baseline / variant** - Metric values for each side (with +/-stdev when multiple time windows)
- **delta** - Absolute difference (variant - baseline)
- **change** - Percentage change
- **sig** - Cohen's d significance marker: ``~`` negligible, ``*`` small, ``**`` medium, ``***`` large

**Presets:**

Presets provide a ready-made hierarchical comparison config covering all annotation
categories for a given workload type. Pass ``--preset <name>`` with ``--baseline``
and ``--variant``.

``dlio``
    Full coverage of DLIO benchmark annotation categories. Produces 8 top-level nodes:

    - **Pipeline** - Epoch-level timing (Epoch, Train, Evaluate, Test)
    - **Data Ingestion** - Data loading paths (Item Loading, Batch Fetch, Reader, DataLoader, Generator)
    - **Checkpointing** - Save and restore paths (Save, Load)
    - **Compute** - AI framework and device operations (Forward/Backward, Framework, Device Transfer)
    - **Storage** - Object store I/O (Read, Write)
    - **POSIX/STDIO I/O** - Low-level syscalls intercepted by dftracer (Read, Write, Metadata, Sync)
    - **Communication** - Distributed collectives
    - **Benchmark** - DLIO orchestration overhead (Lifecycle, Checkpoint I/O, Config)

**JSON config format:**

.. code-block:: json

    {
        "baseline": "./traces_v1",
        "variant": "./traces_v2",
        "defaults": {
            "time_interval_ms": 5000,
            "threshold_pct": 1.0,
            "percentiles": [0.5, 0.95, 0.99]
        },
        "nodes": [
            {
                "name": "POSIX I/O",
                "query": "cat == \"POSIX\"",
                "children": [
                    {"name": "reads", "query": "name == \"read\""},
                    {"name": "writes", "query": "name == \"write\""}
                ]
            }
        ]
    }

