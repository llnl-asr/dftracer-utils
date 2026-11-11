// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DOMAINS_WORKLOAD_REPLAY_REPLAY_H
#define DFTRACER_UTILS_DOMAINS_WORKLOAD_REPLAY_REPLAY_H

#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/domains/workload/replay/trace.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/json/parser.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace dftracer::utils::domains::workload::replay {

/**
 * Configuration options for trace replay
 */
struct ReplayConfig {
    /// Execution options
    bool maintain_timing =
        true;              ///< Maintain original timing between operations
    bool dry_run = false;  ///< Only parse and log operations, don't execute
    bool dftracer_mode = false;  ///< Use DFTracer sleep-based replay mode
    bool no_sleep = false;       ///< Disable sleep calls in dftracer mode
    double timing_scale = 1.0;   ///< Scale timing (1.0 = original, 0.5 = 2x
                                 ///< faster, 2.0 = 2x slower)
    std::uint64_t start_time_offset = 0;  ///< Offset to add to all timestamps
    dstd::String output_directory;  ///< Directory for creating files (empty =
                                    ///< use original paths)
    std::size_t max_file_size =
        1024 * 1024 * 100;          ///< Max file size to create (100MB default)

    /// Function and category filters
    dstd::HashSet<dstd::String>
        filter_functions;  ///< Only replay these functions (empty = all)
    dstd::HashSet<dstd::String> exclude_functions;  ///< Exclude these functions
    dstd::HashSet<dstd::String>
        filter_categories;   ///< Only replay these categories
    dstd::HashSet<dstd::String>
        exclude_categories;  ///< Exclude these categories

    /// Process/Thread filters
    dstd::HashSet<std::uint32_t>
        filter_pids;  ///< Only replay these PIDs (empty = all)
    dstd::HashSet<std::uint32_t>
        filter_tids;  ///< Only replay these TIDs (empty = all)
    dstd::HashSet<std::uint32_t> exclude_pids;  ///< Exclude these PIDs
    dstd::HashSet<std::uint32_t> exclude_tids;  ///< Exclude these TIDs

    /// Timestamp filters (microseconds)
    std::uint64_t start_timestamp =
        0;           ///< Only replay events after this timestamp
    std::uint64_t end_timestamp =
        UINT64_MAX;  ///< Only replay events before this timestamp

    /// Operation size filters
    std::int64_t min_operation_size =
        -1;  ///< Only replay operations >= this size
    std::int64_t max_operation_size =
        -1;  ///< Only replay operations <= this size

    /// Level/depth filter (for hierarchical traces)
    int min_level = -1;  ///< Only replay operations at or above this level
    int max_level = -1;  ///< Only replay operations at or below this level

    /// Sampling options
    double sampling_rate = 1.0;        ///< Replay rate (1.0 = all, 0.1 = 10%)
    std::uint64_t sample_seed = 0;     ///< Random seed for sampling
    bool sample_deterministic = true;  ///< Use deterministic sampling vs random

    /// Resource limits
    std::size_t max_events = 0;  ///< Maximum events to replay (0 = unlimited)
    std::size_t max_open_files = 1024;  ///< Maximum open file descriptors

    /// Call tree options
    bool use_call_tree =
        false;  ///< Use call tree structure for hierarchical replay
    bool hierarchical_replay =
        false;  ///< Replay in hierarchical order (depth-first)
    bool respect_call_hierarchy =
        true;   ///< Respect parent-child relationships in timing

    /// MPI options
    int mpi_rank = 0;  ///< MPI rank of this process
    int mpi_size = 1;  ///< Total number of MPI processes

    /// Optional hook fired after the timing wait and before the executor runs,
    /// to measure dispatch lateness. run_pipelined calls it from several
    /// threads at once.
    dstd::Function<void(const Trace&, std::chrono::steady_clock::time_point)>
        on_dispatch;
};

/**
 * Results and statistics from replay execution
 */
struct ReplayResult {
    std::size_t total_events = 0;
    std::size_t executed_events = 0;
    std::size_t filtered_events = 0;
    std::size_t failed_events = 0;
    std::chrono::microseconds total_duration{0};
    std::chrono::microseconds execution_duration{0};
    /// Keys are non-owning views into the replay @ref dstd::StringIntern pool.
    dstd::HashMap<dstd::StringView, std::size_t> function_counts;
    dstd::HashMap<dstd::StringView, std::size_t> category_counts;
    dstd::Vector<dstd::String> error_messages;

    /// Extended statistics
    dstd::HashMap<std::uint32_t, std::size_t> pid_counts;
    dstd::HashMap<std::uint32_t, std::size_t> tid_counts;
    std::size_t total_bytes_read = 0;
    std::size_t total_bytes_written = 0;
    std::uint64_t first_timestamp = UINT64_MAX;
    std::uint64_t last_timestamp = 0;

    /// Call tree statistics
    std::size_t total_nodes = 0;
    std::size_t tree_depth = 0;
    std::size_t unique_processes = 0;

    /**
     * Print summary statistics
     */
    void print_summary() const;

    /// Adds `other` into this result. The earliest first and latest last
    /// timestamp are kept.
    void merge(const ReplayResult& other);
};

/**
 * Interface for executing individual trace operations
 */
class TraceExecutor {
   public:
    virtual ~TraceExecutor() = default;

    /**
     * Execute a single trace operation
     * @param trace The parsed trace event
     * @param config Replay configuration
     * @return true if successful, false otherwise
     */
    virtual bool execute(const Trace& trace, const ReplayConfig& config) = 0;

    /**
     * Check if this executor can handle the given trace
     * @param trace The trace event to check
     * @return true if this executor can handle the trace
     */
    virtual bool can_handle(const Trace& trace) const = 0;

    /**
     * Get human-readable name for this executor
     */
    virtual dstd::String name() const = 0;
};

/**
 * coro::Executor for POSIX file operations (read, write, open, close, etc.)
 */
class PosixExecutor : public TraceExecutor {
   public:
    bool execute(const Trace& trace, const ReplayConfig& config) override;
    bool can_handle(const Trace& trace) const override;
    dstd::String name() const override { return "POSIX"; }

   private:
    struct OpenFile {
        dstd::File file;
        std::uint64_t position = 0;
    };

    /// Keys are interned via the replay @ref dstd::StringIntern pool.
    dstd::HashMap<dstd::StringView, OpenFile> open_files_;
    /// Scratch buffer reused across reads and writes.
    dstd::Vector<char> io_buffer_;

    bool execute_open(const Trace& trace, const ReplayConfig& config);
    bool execute_close(const Trace& trace, const ReplayConfig& config);
    bool execute_read(const Trace& trace, const ReplayConfig& config);
    bool execute_write(const Trace& trace, const ReplayConfig& config);
    bool execute_seek(const Trace& trace, const ReplayConfig& config);
    bool execute_stat(const Trace& trace, const ReplayConfig& config);

    /// Ensure io_buffer_ has at least `size` bytes; grow with 'A' fill.
    void ensure_io_buffer(std::size_t size);
};

/**
 * DFTracer executor for sleep-based replay
 * Simulates operation timing without actual I/O
 */
class DFTracerExecutor : public TraceExecutor {
   public:
    bool execute(const Trace& trace, const ReplayConfig& config) override;
    bool can_handle(const Trace& trace) const override;
    dstd::String name() const override { return "DFTracer"; }

   private:
    void sleep_for_duration(double duration_microseconds);
};

/**
 * Main replay engine that coordinates trace reading and execution
 *
 * Each input file replays on its own lane (executor and timing anchor), so
 * files from different processes keep their own timelines. max_events caps
 * the whole run.
 *
 * Usage:
 * @code
 *   ReplayConfig config;
 *   config.dftracer_mode = true;
 * @endcode
 *
 *   ReplayEngine engine(config);
 *   auto result = engine.replay("trace.pfw.gz");
 *   result.print_summary();
 */
class ReplayEngine {
   public:
    /**
     * Construct replay engine with configuration
     * @param config Replay configuration options
     */
    explicit ReplayEngine(ReplayConfig config);

    /**
     * Construct and immediately configure for a trace file
     * @param trace_file Path to trace file
     * @param config Replay configuration options
     */
    ReplayEngine(dstd::StringView trace_file, ReplayConfig config);

    ~ReplayEngine();

    /**
     * Replay traces from a single file
     * @param trace_file Path to trace file (.pfw or .pfw.gz)
     * @param index_file Optional path to index file
     * @return Replay results and statistics
     */
    ReplayResult replay(dstd::StringView trace_file,
                        dstd::StringView index_file = "");

    /**
     * Replay traces from multiple files
     * @param trace_files Vector of trace file paths
     * @return Aggregated replay results
     */
    ReplayResult replay(const dstd::Vector<dstd::String>& trace_files);

    /**
     * Replay traces using call tree structure
     * Builds hierarchical call tree and replays in proper order
     * @param trace_dir Directory containing trace files
     * @param pattern File pattern (default: "*.pfw.gz")
     * @return Replay results with call tree statistics
     */
    ReplayResult replay_with_call_tree(dstd::StringView trace_dir,
                                       dstd::StringView pattern = "*.pfw.gz");

    /**
     * Stream parsed Trace events of one trace file. Drives
     * TraceReader::read_json under the hood; each call yields one event. A
     * line that cannot be read yields its error and ends the stream.
     */
    coro::AsyncGenerator<dstd::Result<Trace>> stream_traces(dstd::String file);

    /**
     * Replay files in parallel, one lane per file, at most one file per
     * executor thread, with the file workers spawned on `scope`. Waits of a
     * millisecond or more suspend on the timer. `result` gets the per-file
     * results merged in input order and must outlive the awaited coroutine.
     * Returns the first read error of a file; `result` is then not merged.
     */
    coro::CoroTask<dstd::Result<void>> run_pipelined(
        coro::Scope& scope, const dstd::Vector<dstd::String>& files,
        ReplayResult& result, std::size_t channel_capacity = 4096);

   private:
    struct Lane {
        std::chrono::steady_clock::time_point start{};
        dstd::UniquePtr<TraceExecutor> executor;
        ReplayResult result;
        std::uint64_t first_timestamp = 0;
        bool anchored = false;
    };

    ReplayConfig config_;
    /// Events admitted to execution across all lanes, for max_events.
    std::atomic<std::size_t> admitted_{0};

    Lane make_lane() const;

    /// replay(file) without resetting the max_events count.
    ReplayResult replay_file_sync(dstd::StringView trace_file,
                                  dstd::StringView index_file);

    /**
     * Counts the event into the lane's result. Returns false when it is skipped
     * (filtered or over max_events), otherwise sets `wait` to the time to wait
     * before executing it.
     */
    bool admit(const Trace& trace, Lane& lane, std::chrono::microseconds& wait);

    void execute(const Trace& trace, Lane& lane);

    void replay_one_trace(const Trace& trace, Lane& lane);

    coro::CoroTask<dstd::Result<void>> replay_file(
        dstd::String file, Lane& lane, std::size_t channel_capacity);

    /**
     * Populate a Trace from a parsed JsonParser document.
     */
    bool parse_trace_json(dftracer::utils::json::JsonParser& parser,
                          Trace& trace);

    /**
     * The time to wait before executing `trace`, keeping the lane on its
     * timeline.
     */
    std::chrono::microseconds timing_wait(const Trace& trace, Lane& lane) const;

    /**
     * Check if trace should be executed based on filters
     */
    bool should_execute_trace(const Trace& trace) const;
};

}  // namespace dftracer::utils::domains::workload::replay

#endif  // DFTRACER_UTILS_DOMAINS_WORKLOAD_REPLAY_REPLAY_H
