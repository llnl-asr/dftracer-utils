#ifndef DFTRACER_UTILS_UTILITIES_REPLAY_REPLAY_H
#define DFTRACER_UTILS_UTILITIES_REPLAY_REPLAY_H

#include <dftracer/utils/call_tree/call_tree.h>
#include <dftracer/utils/utilities/reader/internal/line_processor.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>
#include <dftracer/utils/utilities/replay/trace.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::utilities::replay {

/**
 * Configuration options for trace replay
 */
struct ReplayConfig {
    // Execution options
    bool maintain_timing = true;  // Maintain original timing between operations
    bool dry_run = false;        // Only parse and log operations, don't execute
    bool dftracer_mode = false;  // Use DFTracer sleep-based replay mode
    bool no_sleep = false;       // Disable sleep calls in dftracer mode
    double timing_scale =
        1.0;  // Scale timing (1.0 = original, 0.5 = 2x faster, 2.0 = 2x slower)
    std::uint64_t start_time_offset = 0;  // Offset to add to all timestamps
    bool verbose = false;                 // Verbose logging
    std::string output_directory;  // Directory for creating files (empty = use
                                   // original paths)
    std::size_t max_file_size =
        1024 * 1024 * 100;         // Max file size to create (100MB default)

    // Function and category filters
    std::unordered_set<std::string>
        filter_functions;    // Only replay these functions (empty = all)
    std::unordered_set<std::string>
        exclude_functions;   // Exclude these functions
    std::unordered_set<std::string>
        filter_categories;   // Only replay these categories
    std::unordered_set<std::string>
        exclude_categories;  // Exclude these categories

    // Process/Thread filters
    std::unordered_set<std::uint32_t>
        filter_pids;  // Only replay these PIDs (empty = all)
    std::unordered_set<std::uint32_t>
        filter_tids;  // Only replay these TIDs (empty = all)
    std::unordered_set<std::uint32_t> exclude_pids;  // Exclude these PIDs
    std::unordered_set<std::uint32_t> exclude_tids;  // Exclude these TIDs

    // Timestamp filters (microseconds)
    std::uint64_t start_timestamp =
        0;           // Only replay events after this timestamp
    std::uint64_t end_timestamp =
        UINT64_MAX;  // Only replay events before this timestamp

    // Operation size filters
    std::int64_t min_operation_size =
        -1;  // Only replay operations >= this size
    std::int64_t max_operation_size =
        -1;  // Only replay operations <= this size

    // Level/depth filter (for hierarchical traces)
    int min_level = -1;  // Only replay operations at or above this level
    int max_level = -1;  // Only replay operations at or below this level

    // Sampling options
    double sampling_rate = 1.0;        // Replay rate (1.0 = all, 0.1 = 10%)
    std::uint64_t sample_seed = 0;     // Random seed for sampling
    bool sample_deterministic = true;  // Use deterministic sampling vs random

    // Resource limits
    std::size_t max_events = 0;  // Maximum events to replay (0 = unlimited)
    std::size_t max_open_files = 1024;  // Maximum open file descriptors

    // Call tree options
    bool use_call_tree =
        false;  // Use call tree structure for hierarchical replay
    bool hierarchical_replay =
        false;  // Replay in hierarchical order (depth-first)
    bool respect_call_hierarchy =
        true;   // Respect parent-child relationships in timing

    // MPI options
    int mpi_rank = 0;  // MPI rank of this process
    int mpi_size = 1;  // Total number of MPI processes
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
    std::unordered_map<std::string, std::size_t> function_counts;
    std::unordered_map<std::string, std::size_t> category_counts;
    std::vector<std::string> error_messages;

    // Extended statistics
    std::unordered_map<std::uint32_t, std::size_t> pid_counts;
    std::unordered_map<std::uint32_t, std::size_t> tid_counts;
    std::size_t total_bytes_read = 0;
    std::size_t total_bytes_written = 0;
    std::uint64_t first_timestamp = UINT64_MAX;
    std::uint64_t last_timestamp = 0;

    // Call tree statistics
    std::size_t total_nodes = 0;
    std::size_t tree_depth = 0;
    std::size_t unique_processes = 0;

    /**
     * Print summary statistics
     * @param verbose Include detailed breakdown
     */
    void print_summary(bool verbose = false) const;
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
    virtual std::string get_name() const = 0;
};

/**
 * Executor for POSIX file operations (read, write, open, close, etc.)
 */
class PosixExecutor : public TraceExecutor {
   public:
    bool execute(const Trace& trace, const ReplayConfig& config) override;
    bool can_handle(const Trace& trace) const override;
    std::string get_name() const override { return "POSIX"; }

   private:
    std::unordered_map<std::string, int> open_files_;
    int next_fd_ = 1000;

    bool execute_open(const Trace& trace, const ReplayConfig& config);
    bool execute_close(const Trace& trace, const ReplayConfig& config);
    bool execute_read(const Trace& trace, const ReplayConfig& config);
    bool execute_write(const Trace& trace, const ReplayConfig& config);
    bool execute_seek(const Trace& trace, const ReplayConfig& config);
    bool execute_stat(const Trace& trace, const ReplayConfig& config);
};

/**
 * DFTracer executor for sleep-based replay
 * Simulates operation timing without actual I/O
 */
class DFTracerExecutor : public TraceExecutor {
   public:
    bool execute(const Trace& trace, const ReplayConfig& config) override;
    bool can_handle(const Trace& trace) const override;
    std::string get_name() const override { return "DFTracer"; }

   private:
    void sleep_for_duration(double duration_microseconds);
    bool dftracer_initialized_ = false;
};

// Forward declaration
class ReplayLineProcessor;

/**
 * Main replay engine that coordinates trace reading and execution
 *
 * Usage:
 *   ReplayConfig config;
 *   config.dftracer_mode = true;
 *
 *   ReplayEngine engine(config);
 *   auto result = engine.replay("trace.pfw.gz");
 *   result.print_summary();
 */
class ReplayEngine {
    friend class ReplayLineProcessor;

   public:
    /**
     * Construct replay engine with configuration
     * @param config Replay configuration options
     */
    explicit ReplayEngine(const ReplayConfig& config);

    /**
     * Construct and immediately configure for a trace file
     * @param trace_file Path to trace file
     * @param config Replay configuration options
     */
    ReplayEngine(const std::string& trace_file, const ReplayConfig& config);

    ~ReplayEngine();

    /**
     * Add a custom executor for specific trace types
     * @param executor Unique pointer to executor (engine takes ownership)
     */
    void add_executor(std::unique_ptr<TraceExecutor> executor);

    /**
     * Replay traces from a single file
     * @param trace_file Path to trace file (.pfw or .pfw.gz)
     * @param index_file Optional path to index file
     * @return Replay results and statistics
     */
    ReplayResult replay(const std::string& trace_file,
                        const std::string& index_file = "");

    /**
     * Replay traces from multiple files
     * @param trace_files Vector of trace file paths
     * @return Aggregated replay results
     */
    ReplayResult replay(const std::vector<std::string>& trace_files);

    /**
     * Replay traces using call tree structure
     * Builds hierarchical call tree and replays in proper order
     * @param trace_dir Directory containing trace files
     * @param pattern File pattern (default: "*.pfw.gz")
     * @return Replay results with call tree statistics
     */
    ReplayResult replay_with_call_tree(const std::string& trace_dir,
                                       const std::string& pattern = "*.pfw.gz");

   private:
    ReplayConfig config_;
    std::vector<std::unique_ptr<TraceExecutor>> executors_;
    std::chrono::steady_clock::time_point replay_start_time_;
    std::uint64_t first_trace_timestamp_ = 0;
    bool first_timestamp_set_ = false;

    /**
     * Process a single trace line (JSON)
     */
    bool process_trace_line(const std::string& line, ReplayResult& result);

    /**
     * Parse JSON trace into Trace structure
     */
    bool parse_trace_json(const std::string& json_line, Trace& trace);

    /**
     * Apply timing logic before executing trace
     */
    void apply_timing(const Trace& trace);

    /**
     * Check if trace should be executed based on filters
     */
    bool should_execute_trace(const Trace& trace) const;

    /**
     * Find appropriate executor for trace
     */
    TraceExecutor* find_executor(const Trace& trace);

    /**
     * Get file path for replay (handles output directory override)
     */
    std::string get_replay_file_path(const std::string& original_path) const;

    /**
     * Replay from call tree structure (hierarchical replay)
     */
    void replay_from_call_tree(dftracer::utils::call_tree::CallTree& tree,
                               ReplayResult& result);

    /**
     * Replay a single call tree node recursively
     */
    void replay_call_tree_node_recursive(
        const dftracer::utils::call_tree::CallTreeNodeInfo& node,
        dftracer::utils::call_tree::CallTree& call_tree, ReplayResult& result,
        int depth);

    /**
     * Replay a single node from call tree
     */
    void replay_call_tree_node(
        const dftracer::utils::call_tree::CallTreeNodeInfo& node,
        ReplayResult& result);
};

/**
 * Line processor for handling trace lines during replay
 */
class ReplayLineProcessor
    : public dftracer::utils::utilities::reader::internal::LineProcessor {
   public:
    explicit ReplayLineProcessor(ReplayEngine& engine, ReplayResult& result);

    coro::CoroTask<bool> process(const char* data, std::size_t length) override;

   private:
    ReplayEngine& engine_;
    ReplayResult& result_;
};

}  // namespace dftracer::utils::utilities::replay

#endif  // DFTRACER_UTILS_UTILITIES_REPLAY_REPLAY_H
