Replay
==============

The replay utility replays DFTracer trace files by reading recorded events and executing them in a configurable replay mode. It supports plain text and gzipped traces, dry-run analysis, timing-aware replay, and filtered execution for focused testing.

.. note::

   The engine is now pipelined with C++20 coroutines and channels: trace
   reading, JSON parsing, filtering, and execution run as concurrent stages
   communicating through bounded channels, so a slow executor no longer
   blocks the reader. JSON parsing uses the shared
   :cpp:class:`dftracer::utils::utilities::common::json::JsonParser`
   (on-demand simdjson) which reuses one padded buffer per stage. String
   handling and file I/O have been re-tuned with a fixed read buffer and
   ``string_view`` line slicing; the public ``ReplayEngine`` /
   ``ReplayConfig`` / ``ReplayResult`` API is unchanged.

.. code-block:: cpp

   #include <dftracer/utils/utilities/replay/replay.h>
   #include <dftracer/utils/utilities/replay/trace.h>

Overview
--------

The Replay utility is designed to perform the following tasks:

- Parse DFTracer trace files (``.pfw``, ``.pfw.gz``) from one or more files or directories
- Replay events in dry-run mode for validation without issuing I/O
- Reproduce event timing or disable timing for faster execution
- Filter replay by PID, TID, function, category, timestamp range, and operation size
- Limit replay volume with sampling and maximum event counts

Types
-----

.. code-block:: cpp

   // Trace event types
   enum class TraceType {
       Regular,          // Normal function call trace
       FileHash,         // File hash metadata (FH)
       HostHash,         // Host hash metadata (HH)
       StringHash,       // String hash metadata
       ProcessMetadata,  // Process metadata
       OtherMetadata     // Other metadata types
   };

   // Single trace event from DFTracer
   struct Trace {
       std::string cat;        // Category (e.g., "posix", "stdio")
       std::string func_name;  // Function name (e.g., "read", "write")
       double duration;        // Duration in microseconds
       std::uint64_t time_start;
       std::uint64_t time_end;
       std::uint64_t pid;
       std::uint64_t tid;
       std::int64_t size;      // Operation size (-1 = unknown)
       std::int64_t offset;    // File offset (-1 = unknown)
       TraceType type;
       bool is_valid;
   };

   // Replay results and statistics
   struct ReplayResult {
       std::size_t total_events;
       std::size_t executed_events;
       std::size_t filtered_events;
       std::size_t failed_events;
       std::chrono::microseconds total_duration;
       std::unordered_map<std::string, std::size_t> function_counts;
       std::unordered_map<std::string, std::size_t> category_counts;
       void print_summary(bool verbose = false) const;
   };

ReplayEngine
------------

Main replay engine that coordinates trace reading and execution.

**Basic replay:**

.. code-block:: cpp

   ReplayConfig config;
   config.dry_run = true;
   config.verbose = true;

   ReplayEngine engine(config);
   ReplayResult result = engine.replay("trace.pfw.gz");
   result.print_summary();

**Replay with timing:**

.. code-block:: cpp

   ReplayConfig config;
   config.maintain_timing = true;
   config.timing_scale = 2.0;  // 2x slower than original

   ReplayEngine engine(config);
   auto result = engine.replay("trace.pfw.gz");

**Replay multiple files:**

.. code-block:: cpp

   ReplayConfig config;
   config.dftracer_mode = true;  // Sleep-based replay

   ReplayEngine engine(config);
   auto result = engine.replay({
       "rank_0.pfw.gz",
       "rank_1.pfw.gz",
       "rank_2.pfw.gz"
   });
   result.print_summary(true);  // Verbose summary

Filtering
---------

Replay can be restricted to specific processes, functions, categories, and time windows.

**Filter by function and category:**

.. code-block:: cpp

   ReplayConfig config;
   config.filter_functions = {"read", "write", "open"};
   config.filter_categories = {"POSIX"};
   config.exclude_functions = {"close"};

   ReplayEngine engine(config);
   auto result = engine.replay("trace.pfw.gz");

**Filter by PID/TID and timestamp range:**

.. code-block:: cpp

   ReplayConfig config;
   config.filter_pids = {1234, 5678};
   config.start_timestamp = 1000000;   // After 1s
   config.end_timestamp = 5000000;     // Before 5s
   config.min_operation_size = 4096;   // Only ops >= 4KB

   ReplayEngine engine(config);
   auto result = engine.replay("trace.pfw.gz");

**Sampling:**

.. code-block:: cpp

   ReplayConfig config;
   config.sampling_rate = 0.1;         // Replay 10% of events
   config.sample_deterministic = true; // Reproducible sampling
   config.max_events = 10000;          // Cap at 10K events

   ReplayEngine engine(config);
   auto result = engine.replay("trace.pfw.gz");

Call Tree Integration
---------------------

Replay traces using hierarchical call tree structure for depth-first execution.

.. code-block:: cpp

   ReplayConfig config;
   config.use_call_tree = true;
   config.hierarchical_replay = true;
   config.respect_call_hierarchy = true;

   ReplayEngine engine(config);
   auto result = engine.replay_with_call_tree(
       "/path/to/traces",  // Directory containing trace files
       "*.pfw.gz"          // File pattern
   );

   result.print_summary(true);
   // result.total_nodes, result.tree_depth, result.unique_processes

Custom Executors
----------------

Add custom executors for specific trace types beyond the built-in POSIX and DFTracer executors.

.. code-block:: cpp

   class MyExecutor : public TraceExecutor {
   public:
       bool execute(const Trace& trace, const ReplayConfig& config) override {
           // Custom execution logic
           return true;
       }

       bool can_handle(const Trace& trace) const override {
           return trace.cat == "my_category";
       }

       std::string get_name() const override { return "MyExecutor"; }
   };

   ReplayEngine engine(config);
   engine.add_executor(std::make_unique<MyExecutor>());
   auto result = engine.replay("trace.pfw.gz");

Replay Modes
------------

Dry Run
~~~~~~~

Parses trace events and reports replay statistics without performing the underlying operations.

.. code-block:: cpp

   ReplayConfig config;
   config.dry_run = true;

DFTracer Mode
~~~~~~~~~~~~~

Simulates replay using operation durations. Useful for timing-oriented studies without issuing full I/O.

.. code-block:: cpp

   ReplayConfig config;
   config.dftracer_mode = true;
   config.no_sleep = false;  // Set true to skip sleep calls

Direct Replay
~~~~~~~~~~~~~

Executes replay operations directly. The built-in ``PosixExecutor`` handles ``read``, ``write``, ``open``, ``close``, ``seek``, and ``stat``.

.. code-block:: cpp

   ReplayConfig config;
   config.output_directory = "/tmp/replay_output";
   config.max_file_size = 1024 * 1024 * 100;  // 100MB max

Performance Considerations
--------------------------

**Compressed Traces**
  Gzipped traces are supported directly. Replay performance depends on trace size and decompression overhead.

**Sampling and Limits**
  Use sampling and maximum event limits to reduce replay cost for CI, debugging, and exploratory analysis.

**Timing Control**
  Disabling timing can significantly reduce wall-clock runtime when only correctness or parsing behavior needs to be validated.

See Also
--------

- :doc:`/cli` - Command-line tools (``dftracer_replay``)
- :doc:`/call-tree` - Call tree utility
- :doc:`/cpp_api/index` - Full C++ API documentation
