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

**Pipeline** (``PipelineArgs``)

- ``--executor-threads <count>`` - Number of worker threads for parallel
  processing (default: number of CPU cores)
- ``--io-threads <count>`` - Number of I/O threads (default: number of CPU
  cores)
- ``--time-profiling`` - Print stage timing breakdown to stderr

**Indexing** (``IndexingArgs``)

- ``--index-dir <path>`` - Directory for ``.dftindex`` stores
- ``--checkpoint-size <bytes>`` - Checkpoint size for gzip indexing in bytes
  (default: 33554432 B / 32 MB)
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
- ``--files <files...>`` - Trace files (``.pfw``, ``.pfw.gz``)

dftracer_reader
---------------

**Description:** DFTracer utility for reading and indexing compressed files (GZIP, TAR.GZ)

**Usage:**

.. code-block:: bash

   dftracer_reader [OPTIONS] file

**Arguments:**

- ``file`` - Compressed file to process (GZIP, TAR.GZ) [required]

**Options:**

- ``-i, --index <path>`` - Index file to use (default: auto-generated in temp directory)
- ``-s, --start <bytes>`` - Start position in bytes (default: -1)
- ``-e, --end <bytes>`` - End position in bytes (default: -1)
- ``-c, --checkpoint-size <bytes>`` - Checkpoint size for indexing in bytes (default: 33554432 B / 32 MB)
- ``-f, --force-rebuild`` - Force rebuild of index even if it exists
- ``--check`` - Check if index is valid
- ``--read-buffer-size <bytes>`` - Size of the read buffer in bytes (default: 1MB)
- ``--mode <mode>`` - Set the reading mode: bytes, line_bytes, or lines (default: bytes)
- ``--index-dir <path>`` - Directory to store index files (default: system temp directory)

**Example:**

.. code-block:: bash

   # Read bytes 100-200 from a compressed file
   dftracer_reader --start 100 --end 200 trace.pfw.gz

   # Read in line mode
   dftracer_reader --mode lines --start 1 --end 100 trace.pfw.gz

   # Build index with custom checkpoint size
   dftracer_reader --checkpoint-size 20971520 trace.pfw.gz

dftracer_info
-------------

**Description:** Display metadata and index information for DFTracer compressed files

**Usage:**

.. code-block:: bash

   dftracer_info [OPTIONS]

**Options:**

- ``--files <files...>`` - Compressed files to inspect (GZIP, TAR.GZ)
- ``-d, --directory <path>`` - Directory containing files to inspect
- ``--query <type>`` - Query type: ``summary`` (aggregate all files, default) or ``detailed`` (per-file output)
- ``-v, --verbose`` - Show detailed information including index details
- ``-f, --force-rebuild`` - Force rebuild index files
- ``-c, --checkpoint-size <bytes>`` - Checkpoint size for indexing in bytes (default: 33554432 B / 32 MB)
- ``--index-dir <path>`` - Directory to store index files (default: system temp directory)
- ``--executor-threads <count>`` - Number of worker threads for parallel processing (default: number of CPU cores)

**Example:**

.. code-block:: bash

   # Show info for files in a directory
   dftracer_info -d ./logs

   # Show info for specific files with verbose output
   dftracer_info --files trace1.pfw.gz trace2.pfw.gz -v

   # Per-file detailed output
   dftracer_info -d ./traces --query detailed

   # Analyze with 4 threads
   dftracer_info --executor-threads 4 -d ./traces

dftracer_merge
--------------

**Description:** Merge DFTracer .pfw or .pfw.gz files into a single JSON array file using pipeline processing

**Usage:**

.. code-block:: bash

   dftracer_merge [OPTIONS]

**Options:**

- ``-d, --directory <path>`` - Directory containing .pfw or .pfw.gz files (default: .)
- ``-o, --output <path>`` - Output file path (should have .pfw extension) (default: combined.pfw)
- ``-f, --force`` - Override existing output file and force index recreation
- ``-c, --compress`` - Compress output file with gzip
- ``-v, --verbose`` - Enable verbose mode
- ``-g, --gzip-only`` - Process only .pfw.gz files
- ``--checkpoint-size <bytes>`` - Checkpoint size for indexing in bytes (default: 33554432 B / 32 MB)
- ``--executor-threads <count>`` - Number of worker threads for parallel processing (default: number of CPU cores)
- ``--index-dir <path>`` - Directory to store index files (default: system temp directory)

**Example:**

.. code-block:: bash

   # Merge all .pfw/.pfw.gz files in current directory
   dftracer_merge -o merged.pfw

   # Merge files from specific directory with compression
   dftracer_merge -d ./logs -o output.pfw -c

   # Merge with parallel processing and verbose output
   dftracer_merge -d ./traces -o combined.pfw --executor-threads 8 -v

dftracer_split
--------------

**Description:** Split DFTracer traces into equal-sized chunks using pipeline processing

**Usage:**

.. code-block:: bash

   dftracer_split [OPTIONS]

**Options:**

- ``-n, --app-name <name>`` - Application name for output files (default: app)
- ``-d, --directory <path>`` - Input directory containing .pfw or .pfw.gz files (default: .)
- ``-o, --output <dir>`` - Output directory for split files (default: ./split)
- ``-s, --chunk-size <MB>`` - Chunk size in MB (default: 4)
- ``-f, --force`` - Override existing files and force index recreation
- ``-c, --compress`` - Compress output files with gzip (default: true)
- ``-v, --verbose`` - Enable verbose mode
- ``--checkpoint-size <bytes>`` - Checkpoint size for indexing in bytes (default: 33554432 B / 32 MB)
- ``--executor-threads <count>`` - Number of worker threads for parallel processing (default: number of CPU cores)
- ``--index-dir <path>`` - Directory to store index files (default: system temp directory)
- ``--verify`` - Verify output chunks match input by comparing event IDs

**Example:**

.. code-block:: bash

   # Split files into 4MB chunks
   dftracer_split -d ./logs -o ./split_output

   # Split with 10MB chunks and custom app name
   dftracer_split -d ./traces -s 10 -n myapp -o ./chunks

   # Split without compression and verify output
   dftracer_split -d ./data -c false --verify -o ./output

dftracer_event_count
--------------------

**Description:** Count valid events in DFTracer .pfw or .pfw.gz files using pipeline processing

**Usage:**

.. code-block:: bash

   dftracer_event_count [OPTIONS]

**Options:**

- ``-d, --directory <path>`` - Directory containing .pfw or .pfw.gz files (default: .)
- ``-f, --force`` - Force index recreation
- ``-c, --checkpoint-size <bytes>`` - Checkpoint size for indexing in bytes (default: 33554432 B / 32 MB)
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

dftracer_pgzip
--------------

**Description:** Parallel gzip compression for DFTracer .pfw files

**Usage:**

.. code-block:: bash

    dftracer_pgzip [OPTIONS]

**Options:**

- ``-d, --directory <path>`` - Directory containing .pfw files (default: .)
- ``-v, --verbose`` - Enable verbose output
- ``--executor-threads <count>`` - Number of worker threads for parallel processing (default: number of CPU cores)

**Example:**

.. code-block:: bash

    # Compress all .pfw files in current directory
    dftracer_pgzip

    # Compress files in specific directory with verbose output
    dftracer_pgzip -d ./logs -v

    # Compress with 16 threads
    dftracer_pgzip -d ./traces --executor-threads 16

dftracer_server
---------------

**Description:** HTTP server for querying and streaming DFTracer trace data via REST API

**Usage:**

.. code-block:: bash

     dftracer_server [OPTIONS] --directory <path>

**Options:**

- ``-b, --bind <address>`` - Bind address (default: 0.0.0.0)
- ``-p, --port <number>`` - Listen port (default: 8080)
- ``-d, --directory <path>`` - Directory containing trace files [required]
- ``--index-dir <path>`` - Directory for bloom/checkpoint index files (default: same as --directory)
- ``--executor-threads <count>`` - Number of worker threads (default: number of CPU cores)

**Example:**

.. code-block:: bash

     # Start server on default port 8080
     dftracer_server -d ./traces

     # Start server on custom port with specific bind address
     dftracer_server -b 127.0.0.1 -p 9000 -d ./traces

     # Start with custom index directory and thread count
     dftracer_server -d ./traces --index-dir /var/cache/dftracer_indexes --executor-threads 8

dftracer_stats
--------------

**Description:** Compute event statistics with bloom filter acceleration and detailed distribution analysis

**Usage:**

.. code-block:: bash

    dftracer_stats [OPTIONS]

**Options:**

- ``-d, --directory <path>`` - Directory containing .pfw or .pfw.gz files (default: .)
- ``--files <files...>`` - Explicit list of trace files
- ``--index-dir <path>`` - Directory to store index files (default: system temp directory)
- ``--report <type>`` - Report type: summary, categories, names, pid_tids, time_range, duration, top-names, top-categories, detailed (default: summary)
- ``--top-n <count>`` - Top N entries to show in detailed report (0=all, default: 10)
- ``--top-n-pid-tid <count>`` - Top N PID:TID pairs to show (default: 10)
- ``--query <query>`` - Query DSL filter (e.g., ``'cat == "POSIX" and dur > 1000'``)
- ``--group-by <dims...>`` - Group-by dimensions: name, cat, pid, tid, fhash, hhash, pid_tid (default: name for detailed)
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

- ``--files <files...>`` - Trace files to process (.pfw, .pfw.gz)
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
- ``--index-dir <path>`` - Directory where .idx index files are stored
- ``--no-auto-index`` - Disable automatic bloom index building for files missing .idx
- ``--checkpoint-size <bytes>`` - Checkpoint size for auto-indexing in bytes (default: 33554432 B / 32 MB)
- ``--executor-threads <count>`` - Number of worker threads (default: number of CPU cores)

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

- ``-d, --directory <path>`` - Input directory containing .pfw or .pfw.gz files (default: .)
- ``--dimensions <dims>`` - Comma-separated extra dimensions to index from args (e.g., args.level,args.mode)
- ``-f, --force`` - Force index recreation even if already built
- ``--checkpoint-size <bytes>`` - Checkpoint size for gzip indexing in bytes (default: 33554432 B / 32 MB)
- ``--executor-threads <count>`` - Number of worker threads for parallel processing (default: number of CPU cores)
- ``--index-dir <path>`` - Directory to store index files (default: same as data files)
- ``--expected-entries <count>`` - Expected entries per chunk for bloom filter sizing (default: 1024)
- ``--false-positive-rate <rate>`` - Bloom filter false positive rate (default: 0.01)
- ``--read-batch-size <MB>`` - Batch read size in MB for stream processing (default: 4)
- ``--manifest`` - Also build manifest tables in .idx (per-checkpoint event line routing)
- ``--rebuild-summaries`` - Rebuild ``ROOT_*`` aggregated summaries after ingest.
  Off by default; ``ROOT_*`` CFs are only consumed by summary tools such as
  ``dftracer_info``. Bloom-filter chunk-skipping queries do not require them.

This binary also accepts the shared :ref:`cli-shared-flags` (Pipeline,
Watchdog, Indexing).

**Example:**

.. code-block:: bash

    # Build bloom indices for all traces
    dftracer_index -d ./traces

    # Build with custom dimensions and force rebuild
    dftracer_index -d ./traces --dimensions "args.level,args.io.size" --force

    # Build manifest indices for reorganization
    dftracer_index -d ./traces --manifest

dftracer_aggregator
-------------------

**Description:** Aggregate DFTracer events into time-series counters using streaming coroutine pipeline

The aggregator can emit three logical row types:

- regular event rows from non-counter trace events
- profile-counter rows from ``ph="C"`` events whose category is not ``sys``
- system-counter rows from ``ph="C"`` events whose category is ``sys``

With ``--format arrow``, these are distinguished by the ``batch_type`` column.
The Arrow output always includes the base columns ``batch_type``, ``cat``,
``name``, ``pid``, ``tid``, ``hhash``, ``fhash``, ``time_bucket``, ``count``,
``dur_total``, ``dur_min``, ``dur_max``, ``dur_mean``, ``dur_std``,
``size_total``, ``size_min``, ``size_max``, ``size_mean``, ``size_std``,
``ts``, and ``te``. Each field listed in ``--metric-fields`` adds
``<field>_total``, ``<field>_min``, ``<field>_max``, ``<field>_mean``, and
``<field>_std``.

**Usage:**

.. code-block:: bash

    dftracer_aggregator [OPTIONS]

**Options:**

- ``-d, --directory <path>`` - Input directory containing .pfw or .pfw.gz files (default: .)
- ``-o, --output <path>`` - Output file path for aggregated counters (default: aggregated_output.json)
- ``-t, --time-interval <ms>`` - Time interval in milliseconds for bucketing (default: 5000)
- ``-g, --group-keys <keys>`` - Comma-separated extra group keys from args (e.g., epoch,step,level)
- ``-m, --metric-fields <fields>`` - Comma-separated custom metric fields from args (e.g., iter_count,num_events)
- ``--query <query>`` - Query DSL filter (e.g., ``'cat == "POSIX" and dur > 1000'``)
- ``-f, --force`` - Force index recreation
- ``--checkpoint-size <bytes>`` - Checkpoint size for indexing in bytes (default: 33554432 B / 32 MB)
- ``--executor-threads <count>`` - Number of executor threads for parallel processing (default: number of CPU cores)
- ``--index-dir <path>`` - Directory to store index files (default: system temp directory)
- ``--compress`` - Compress output using gzip
- ``--compression-level <0-9>`` - Gzip compression level (default: 6)
- ``--boundary-events <config>`` - Boundary event configuration: event_name:value_field:output_name
- ``--no-track-process-parents`` - Disable tracking of process parent relationships from fork/spawn
- ``--chunk-size <MB>`` - Target chunk size in MB for parallel processing (default: 4)
- ``--read-batch-size <MB>`` - Batch read size in MB for stream processing (default: 4)
- ``--event-format <fmt>`` - Perfetto event format: counter, async, regular (default: counter)
- ``--compute-percentiles`` - Enable percentile/quantile computation using DDSketch
- ``--percentiles <vals>`` - Comma-separated percentiles to compute (e.g., 0.25,0.5,0.75,0.90)
- ``--relative-accuracy <rate>`` - Relative accuracy for DDSketch percentile estimation (default: 0.01)
- ``--format <fmt>`` - Output format: ``json`` (default, Perfetto trace) or ``arrow`` (``.arrows`` IPC file). Arrow format requires ``DFTRACER_UTILS_ENABLE_ARROW_IPC=ON`` at build time.

**Example:**

.. code-block:: bash

    # Basic aggregation with 1-second (1000ms) buckets
    dftracer_aggregator -d ./traces -o agg.json -t 1000

    # Aggregation with percentiles and compression
    dftracer_aggregator -d ./traces -o agg.json --compute-percentiles --compress

    # Query-filtered aggregation with custom metrics from args
    dftracer_aggregator -d ./traces --query 'cat == "POSIX"' \
        -m "iter_count,epoch"

    # Output as Arrow IPC file (readable by pyarrow, polars, DuckDB)
    dftracer_aggregator -d ./traces -o agg.arrows --format arrow

    # Stream profile/system counters as Perfetto counter events
    dftracer_aggregator -d ./traces --event-format counter

**Reading Arrow IPC output:**

.. code-block:: python

    # pyarrow
    import pyarrow.ipc as ipc
    reader = ipc.open_file("agg.arrows")
    table = reader.read_all()
    df = table.to_pandas()

    # polars
    import polars as pl
    df = pl.read_ipc("agg.arrows")

    # DuckDB
    import duckdb
    result = duckdb.sql("SELECT * FROM 'agg.arrows'")

dftracer_gen_dlio_config
------------------------

**Description:** Generate a DLIO YAML configuration directly from a directory
of raw DFTracer traces. The tool indexes the inputs, aggregates them into the
internal ``AGGREGATION`` column family (DDSketch forced on), fits per-component
distributions, refines ``max_bound`` against an internal barrier simulator, and
emits a DLIO ``train.computation_time`` + ``reader.preprocess_time`` block. The
user does not need to run ``dftracer_aggregator`` separately.

Required input event names: ``cat=dataloader`` with ``name=fetch.block`` /
``fetch.iter``, and ``cat=data`` with ``name=preprocess`` / ``item``. The tool
exits non-zero with an explanatory message if no DLIO events are present.

**Usage:**

.. code-block:: bash

    dftracer_gen_dlio_config [OPTIONS] -o <config.yaml>

**Options:**

- ``-d, --directory <path>`` - Input directory containing .pfw or .pfw.gz traces (default: .)
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
- ``--index-dir <path>`` - Directory for the shared index store (default: system temp dir)
- ``--checkpoint-size <bytes>`` - Checkpoint size for indexing in bytes (default: 33554432 B / 32 MB)
- ``--executor-threads <count>`` - Number of executor threads for parallel processing
- ``-f, --force`` - Force index recreation

**Distribution pool:** Each component is fit as the lowest-BIC choice among
{Normal, Lognormal, Gamma, Exponential, Weibull, Gaussian Mixture (K=2),
Gaussian Mixture (K=3)}. Mixture candidates are only considered when the
sample count is at least 20.

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

dftracer_organize
-----------------

**Description:** Reorganize traces by routing events to query-based groups with provenance tracking

**Usage:**

.. code-block:: bash

    dftracer_organize [OPTIONS] --output <dir> --groups <groups...>

**Options:**

- ``--files <files...>`` - Input trace files (.pfw, .pfw.gz)
- ``-d, --directory <path>`` - Directory containing trace files
- ``-o, --output <dir>`` - Output directory [required]
- ``--groups <groups...>`` - Query groups: ``'io:cat == "POSIX"'`` ``'compute:cat == "APP"'`` [required]
- ``--chunk-size <MB>`` - Target chunk size in MB for output files (default: 256)
- ``--checkpoint-size <bytes>`` - Checkpoint size for indexing in bytes (default: 33554432 B / 32 MB)
- ``--index-dir <path>`` - Directory for sidecar files
- ``-f, --force`` - Force rebuild of indices
- ``--no-compress`` - Write plain .pfw instead of .pfw.gz
- ``--executor-threads <count>`` - Worker threads (default: number of CPU cores)

**Example:**

.. code-block:: bash

    # Separate I/O and compute operations
    dftracer_organize -d ./traces -o ./organized \
        --groups 'io:cat == "POSIX"' 'compute:cat == "APP"'

    # Create multiple semantic views
    dftracer_organize -d ./traces -o ./views \
        --groups 'read:name == "read"' 'write:name == "write"' 'other:'

    # Keep uncompressed output
    dftracer_organize -d ./traces -o ./plain --groups "all:" --no-compress

dftracer_reconstruct
--------------------

**Description:** Reconstruct original traces from reorganized files using provenance tracking in .pidx sidecars

**Usage:**

.. code-block:: bash

    dftracer_reconstruct [OPTIONS] --directory <dir> --output <dir>

**Options:**

- ``-d, --directory <path>`` - Directory containing reorganized files [required]
- ``-o, --output <dir>`` - Output directory [required]
- ``--index-dir <path>`` - Directory for sidecar files
- ``--checkpoint-size <bytes>`` - Checkpoint size for indexing in bytes (default: 33554432 B / 32 MB)
- ``--no-compress`` - Write plain .pfw instead of .pfw.gz
- ``--executor-threads <count>`` - Worker threads (default: number of CPU cores)

**Example:**

.. code-block:: bash

    # Reconstruct from reorganized directory
    dftracer_reconstruct -d ./organized -o ./reconstructed

    # Reconstruct without compression
    dftracer_reconstruct -d ./views -o ./reconstructed --no-compress

dftracer_replay
---------------

**Description:** Replay I/O operations from DFTracer trace files with timing and filtering support

**Usage:**

.. code-block:: bash

    dftracer_replay [OPTIONS] <inputs...>

**Options:**

- ``inputs`` - Trace files (.pfw, .pfw.gz) or directories containing trace files [required]
- ``--no-timing`` - Ignore original timing and execute as fast as possible
- ``--dry-run`` - Parse and analyze traces without executing operations
- ``--dftracer-mode`` - Use DFTracer sleep-based replay (sleep for operation duration instead of doing actual I/O)
- ``--no-sleep`` - When used with --dftracer-mode, disable sleep calls for maximum speed
- ``--verbose`` - Enable verbose output and detailed statistics
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

**Example:**

.. code-block:: bash

    # Replay with original timing
    dftracer_replay ./traces/rank_0.pfw.gz

    # Dry-run analysis of trace file
    dftracer_replay ./traces/rank_0.pfw.gz --dry-run --verbose

    # Replay only POSIX read operations
    dftracer_replay -d ./traces -r --filter-category POSIX --filter-function read

For detailed usage, see :doc:`utilities/replay`.

dftracer_tar
------------

**Description:** Index and analyze TAR.GZ archives containing DFTracer trace data

**Usage:**

.. code-block:: bash

    dftracer_tar [OPTIONS] <file>

**Options:**

- ``file`` - TAR.GZ file to process [required]
- ``-i, --index <path>`` - Index file to use (auto-generated if not specified)
- ``-c, --checkpoint-size <bytes>`` - Checkpoint size for indexing in bytes (default: 33554432 B / 32 MB)
- ``-f, --force-rebuild`` - Force rebuild index
- ``--list-files`` - List all files in the TAR archive
- ``--info`` - Show archive information
- ``--build-only`` - Only build the index, don't perform other operations

**Example:**

.. code-block:: bash

    # Show archive information
    dftracer_tar trace_archive.tar.gz --info

    # List files in archive
    dftracer_tar trace_archive.tar.gz --list-files

    # Build index for fast access
    dftracer_tar trace_archive.tar.gz --build-only

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

dftracer_call_tree
------------------

**Description:** Build and analyze call trees from DFTracer trace files for hierarchical structure analysis

**Usage:**

.. code-block:: bash

    dftracer_call_tree [OPTIONS] <inputs...>

**Options:**

- ``inputs`` - Trace files (.pfw, .pfw.gz) or directories containing trace files [required]
- ``-r, --recursive`` - Recursively search directories for trace files
- ``--pattern <pattern>`` - File pattern for trace files (default: ``*.pfw.gz``)
- ``-o, --output <path>`` - Output file path for serialized call tree (auto-generated from input if not specified)
- ``--json`` - Also save call tree in JSON (Chrome Tracing) format
- ``--text <path>`` - Export call tree to text file
- ``--max-depth <n>`` - Maximum depth for tree printing (0=unlimited, default: 0)
- ``--analyze`` - Perform detailed analysis (call patterns, timing, critical path)
- ``-v, --verbose`` - Enable verbose output
- ``--stats-only`` - Only print statistics, skip tree traversal
- ``--no-save`` - Don't save output files, only print analysis

**Example:**

.. code-block:: bash

    # Build call tree from directory
    dftracer_call_tree ./traces --analyze

    # Export to JSON and text formats
    dftracer_call_tree ./traces --json --text tree.txt

    # Analyze with detailed statistics
    dftracer_call_tree ./traces --analyze --verbose --max-depth 5

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
- ``--force`` - Force index rebuild
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

- **baseline / variant** - Metric values for each side (with ±stdev when multiple time windows)
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

dftracer_aggregator_mpi
-----------------------

**Description:** MPI driver for the distributed-SST aggregator. Each rank
produces per-rank aggregation SSTs; rank 0 bulk-ingests and the ranks jointly
write the final gzip JSON output. Requires the build to be configured with
``DFTRACER_UTILS_ENABLE_MPI=ON``.

The pipeline is structured as a five-task DAG executed inside the standard
``Pipeline`` runtime:

``scan -> phase_a -> phase_b -> phase_c -> merge``

- **scan** - Cooperative gzip-member pre-scan, ``Allgatherv`` of the member
  map, and deterministic Longest-Processing-Time (LPT) assignment of work
  units to ranks.
- **phase_a** - Each rank runs the distributed-SST indexer + aggregation
  visitor on its slice and writes SSTs (and ``tracker.bin``) to its rank
  staging directory. SSTs are optionally moved to a shared-FS staging root
  for the coordinator.
- **phase_b** - Rank 0 ``Gatherv`` of artifact lists and a single
  ``IndexDatabase::bulk_ingest`` + tracker merge.
- **phase_c** - Each rank writes a shard-prefixed Perfetto gzip JSON slice
  using ``PerfettoTraceWriterUtility``.
- **merge** - Parallel ``pwrite`` on Lustre-striped output or serial
  concatenation otherwise.

**Usage:**

.. code-block:: bash

    mpirun -n <N> dftracer_aggregator_mpi [OPTIONS]

**Options:**

- ``-d, --directory <path>`` - Input directory containing .pfw or .pfw.gz
  files (default: ``.``)
- ``-o, --output <path>`` - Output gzip JSON path. ``.gz`` is appended if
  missing (default: ``aggregated_output.json.gz``)
- ``-t, --time-interval <ms>`` - Time interval in milliseconds for bucketing
  (default: 5000)
- ``--staging-dir <path>`` - Per-rank SST staging root. Defaults to
  ``<index_dir>/_staging``; each rank writes to ``<staging_dir>/rank_<R>``.
- ``--shared-staging <path>`` - Shared-FS staging root. When set and
  different from ``--staging-dir``, each rank moves its SSTs and
  ``tracker.bin`` from the (node-local) staging dir to
  ``<shared-staging>/rank_<R>`` before the coordinator ingest. Required for
  multi-node runs where ``--staging-dir`` points at node-local NVMe.
- ``--keep-staging`` - Keep per-rank SST staging dirs after a successful
  ingest

This binary also accepts the shared :ref:`cli-shared-flags` (Pipeline and
Indexing schemas). Per-rank ``--executor-threads`` / ``--io-threads`` are
automatically scaled down by the detected processes-per-node count so
co-located ranks do not oversubscribe cores.

**Example:**

.. code-block:: bash

    # 16 ranks on one node, node-local staging
    mpirun -n 16 dftracer_aggregator_mpi -d ./traces -o agg.json.gz

    # Multi-node run with shared staging on Lustre
    mpirun -n 64 dftracer_aggregator_mpi -d /lustre/traces \
        --staging-dir /local/nvme/_staging \
        --shared-staging /lustre/scratch/_staging \
        -o /lustre/out/agg.json.gz

dftracer_call_tree_mpi
----------------------

**Description:** MPI driver for parallel call-tree construction. Each rank
owns a slice of PIDs, emits a Chrome Tracing JSON shard, and rank 0 merges
the shards. Wraps the ``MPICallTreeBuilder`` engine
(``discover_pids -> build -> hierarchy -> write -> merge`` coro phases).
Requires ``DFTRACER_UTILS_ENABLE_MPI=ON``.

**Usage:**

.. code-block:: bash

    mpirun -n <N> dftracer_call_tree_mpi [OPTIONS] <input>

**Options:**

- ``input`` - Input directory containing trace files [required]
- ``-o, --output <path>`` - Output JSON path (default: ``call_tree.pfw``)
- ``--staging-dir <path>`` - Shared-FS staging root for per-rank shards
  (default: ``<output>.shards/``)
- ``--gzip`` - gzip the merged output (``.gz`` appended if needed)
- ``-v, --verbose`` - Verbose progress logging
- ``--keep-staging`` - Keep per-rank shard files after merge

This binary also accepts the shared :ref:`cli-shared-flags` (Pipeline);
per-rank thread counts are scaled down by the detected processes-per-node
count.

**Example:**

.. code-block:: bash

    # 32 ranks across nodes; gzip merged output
    mpirun -n 32 dftracer_call_tree_mpi ./traces -o call_tree.pfw --gzip
