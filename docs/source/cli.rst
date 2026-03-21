Command-Line Tools
==================

DFTracer Utils provides several command-line utilities for working with DFTracer trace files and compressed archives.

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
- ``-v, --verbose`` - Show detailed information including index details
- ``-f, --force-rebuild`` - Force rebuild index files
- ``-c, --checkpoint-size <bytes>`` - Checkpoint size for indexing in bytes (default: 33554432 B / 32 MB)
- ``--index-dir <path>`` - Directory to store index files (default: system temp directory)
- ``--threads <count>`` - Number of threads for parallel processing (default: number of CPU cores)

**Example:**

.. code-block:: bash

   # Show info for files in a directory
   dftracer_info -d ./logs

   # Show info for specific files with verbose output
   dftracer_info --files trace1.pfw.gz trace2.pfw.gz -v

   # Analyze with 4 threads
   dftracer_info --threads 4 -d ./traces

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
- ``--threads <count>`` - Number of threads for parallel processing (default: number of CPU cores)
- ``--index-dir <path>`` - Directory to store index files (default: system temp directory)

**Example:**

.. code-block:: bash

   # Merge all .pfw/.pfw.gz files in current directory
   dftracer_merge -o merged.pfw

   # Merge files from specific directory with compression
   dftracer_merge -d ./logs -o output.pfw -c

   # Merge with parallel processing and verbose output
   dftracer_merge -d ./traces -o combined.pfw --threads 8 -v

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
- ``--threads <count>`` - Number of threads for parallel processing (default: number of CPU cores)
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
- ``--threads <count>`` - Number of threads for parallel processing (default: number of CPU cores)
- ``--index-dir <path>`` - Directory to store index files (default: system temp directory)

**Example:**

.. code-block:: bash

   # Count events in current directory
   dftracer_event_count

   # Count events in specific directory with 8 threads
   dftracer_event_count -d ./traces --threads 8

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
- ``--threads <count>`` - Number of threads for parallel processing (default: number of CPU cores)

**Example:**

.. code-block:: bash

    # Compress all .pfw files in current directory
    dftracer_pgzip

    # Compress files in specific directory with verbose output
    dftracer_pgzip -d ./logs -v

    # Compress with 16 threads
    dftracer_pgzip -d ./traces --threads 16

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
- ``--query <query...>`` - Bloom filter queries for chunk-skipping (e.g., name=read,cat=POSIX)
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
    dftracer_stats -d ./traces --report duration --query cat=POSIX

dftracer_view
-------------

**Description:** Extract filtered subsets of trace data using bloom-accelerated views and predicates

**Usage:**

.. code-block:: bash

    dftracer_view [OPTIONS]

**Options:**

- ``--files <files...>`` - Trace files to process (.pfw, .pfw.gz)
- ``-d, --directory <path>`` - Directory containing trace files
- ``--preset <name>`` - Predefined view: io, compute, dlio
- ``--recipe <path>`` - Custom view JSON file path
- ``--save-recipe <path>`` - Save the constructed view to a JSON file
- ``--query <query...>`` - Inline query (e.g., cat=POSIX,name=read|write)
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
    dftracer_view -d ./traces --query "cat=POSIX" "name=read|write" -o posix_rw.pfw

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

**Usage:**

.. code-block:: bash

    dftracer_aggregator [OPTIONS]

**Options:**

- ``-d, --directory <path>`` - Input directory containing .pfw or .pfw.gz files (default: .)
- ``-o, --output <path>`` - Output file path for aggregated counters (default: aggregated_output.json)
- ``-t, --time-interval <sec>`` - Time interval in seconds for bucketing (default: 5.0)
- ``-g, --group-keys <keys>`` - Comma-separated extra group keys from args (e.g., epoch,step,level)
- ``-m, --metric-fields <fields>`` - Comma-separated custom metric fields from args (e.g., iter_count,num_events)
- ``-c, --categories <cats>`` - Include only these categories (comma-separated, empty = all)
- ``-n, --names <names>`` - Include only these event names (comma-separated, empty = all)
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

    # Basic aggregation with 1-second buckets
    dftracer_aggregator -d ./traces -o agg.json -t 1.0

    # Aggregation with percentiles and compression
    dftracer_aggregator -d ./traces -o agg.json --compute-percentiles --compress

    # Filter to specific categories with custom metrics
    dftracer_aggregator -d ./traces -c "POSIX,APP" -m "iter_count,epoch"

    # Output as Arrow IPC file (readable by pyarrow, polars, DuckDB)
    dftracer_aggregator -d ./traces -o agg.arrows --format arrow

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

dftracer_organize
-----------------

**Description:** Reorganize traces by routing events to predicate-based groups with provenance tracking

**Usage:**

.. code-block:: bash

    dftracer_organize [OPTIONS] --output <dir> --groups <predicates...>

**Options:**

- ``--files <files...>`` - Input trace files (.pfw, .pfw.gz)
- ``-d, --directory <path>`` - Directory containing trace files
- ``-o, --output <dir>`` - Output directory [required]
- ``--groups <predicates...>`` - Predicate groups: "io:cat=POSIX" "compute:cat=APP" [required]
- ``--checkpoint-size <bytes>`` - Checkpoint size for indexing in bytes (default: 33554432 B / 32 MB)
- ``--index-dir <path>`` - Directory for sidecar files
- ``-f, --force`` - Force rebuild of indices
- ``--no-compress`` - Write plain .pfw instead of .pfw.gz
- ``--executor-threads <count>`` - Worker threads (default: number of CPU cores)

**Example:**

.. code-block:: bash

    # Separate I/O and compute operations
    dftracer_organize -d ./traces -o ./organized --groups "io:cat=POSIX" "compute:cat=APP"

    # Create multiple semantic views
    dftracer_organize -d ./traces -o ./views --groups "read:name=read" "write:name=write" "other:"

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
