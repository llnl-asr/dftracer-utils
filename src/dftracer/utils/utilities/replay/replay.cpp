#include <dftracer/utils/call_tree/call_tree.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/utilities/common/json/parser.h>
#include <dftracer/utils/utilities/composites/dft/event.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <dftracer/utils/utilities/replay/replay.h>

#include <chrono>
#include <random>
#include <thread>

namespace dftracer::utils::utilities::replay {

namespace {

// Process-wide intern pool for replay strings. Function names, categories,
// and per-file hashes (fhash/hhash) have either small bounded cardinality
// (~tens for cat/name) or stable identity per file (fhash).
dftracer::utils::StringIntern& replay_intern() {
    static dftracer::utils::StringIntern instance;
    return instance;
}

std::string_view intern_sv(std::string_view sv) {
    return replay_intern().intern(sv);
}

}  // anonymous namespace

// =============================================================================
// ReplayEngine Implementation
// =============================================================================

ReplayEngine::ReplayEngine(const ReplayConfig& config)
    : config_(config), replay_start_time_(std::chrono::steady_clock::now()) {
    if (config.dftracer_mode) {
        add_executor(std::make_unique<DFTracerExecutor>());
    } else {
        add_executor(std::make_unique<PosixExecutor>());
    }
}

ReplayEngine::ReplayEngine(const std::string& /* trace_file */,
                           const ReplayConfig& config)
    : config_(config), replay_start_time_(std::chrono::steady_clock::now()) {
    if (config.dftracer_mode) {
        add_executor(std::make_unique<DFTracerExecutor>());
    } else {
        add_executor(std::make_unique<PosixExecutor>());
    }
}

ReplayEngine::~ReplayEngine() = default;

void ReplayEngine::add_executor(std::unique_ptr<TraceExecutor> executor) {
    executors_.push_back(std::move(executor));
}

coro::AsyncGenerator<Trace> ReplayEngine::stream_traces(
    const std::vector<std::string>& files) {
    using reader::ReadConfig;
    using reader::TraceReader;
    using reader::TraceReaderConfig;

    for (const auto& file : files) {
        TraceReaderConfig cfg;
        cfg.file_path = file;
        cfg.auto_build_index = true;
        TraceReader rdr(std::move(cfg));
        auto gen = rdr.read_json(ReadConfig{});
        while (auto opt = co_await gen.next()) {
            if (!opt->parser) continue;
            Trace trace;
            if (parse_trace_json(*opt->parser, trace)) {
                co_yield std::move(trace);
            }
        }
    }
}

coro::CoroTask<void> ReplayEngine::run_pipelined(
    dftracer::utils::CoroScope& scope, const std::vector<std::string>& files,
    ReplayResult& result, std::size_t channel_capacity) {
    coro::Channel<Trace> ch_instance(channel_capacity);
    auto* channel = &ch_instance;

    co_await scope.scope([this, channel, &files,
                          &result](dftracer::utils::CoroScope& child)
                             -> coro::CoroTask<void> {
        // Producer
        child.spawn([this, channel, &files](
                        dftracer::utils::CoroScope&) -> coro::CoroTask<void> {
            auto producer = channel->producer();
            auto guard = producer.guard();
            auto gen = stream_traces(files);
            while (auto trace = co_await gen.next()) {
                if (!co_await producer.send(std::move(*trace))) {
                    co_return;
                }
            }
            co_return;
        });

        // Consumer
        child.spawn([this, channel, &result](
                        dftracer::utils::CoroScope&) -> coro::CoroTask<void> {
            auto consumer = channel->consumer();
            while (auto item = co_await consumer.receive()) {
                dispatch_trace(*item, result);
            }
            co_return;
        });
        co_return;
    });

    co_return;
}

namespace {

// Sync drive used by the existing replay(file)/replay(vector) entry points.
// Pipeline-driven callers use ReplayEngine::run_pipelined instead.
coro::CoroTask<void> replay_file_async(ReplayEngine* engine,
                                       std::string trace_file,
                                       std::string index_file,
                                       ReplayResult* result) {
    using reader::ReadConfig;
    using reader::TraceReader;
    using reader::TraceReaderConfig;

    TraceReaderConfig cfg;
    cfg.file_path = std::move(trace_file);
    if (!index_file.empty()) {
        cfg.index_dir = std::move(index_file);
    }
    cfg.auto_build_index = true;

    TraceReader rdr(std::move(cfg));
    auto gen = rdr.read_json(ReadConfig{});
    while (auto opt = co_await gen.next()) {
        if (!opt->parser) continue;
        engine->process_trace_line(*opt->parser, *result);
    }
    co_return;
}

}  // namespace

ReplayResult ReplayEngine::replay(const std::string& trace_file,
                                  const std::string& index_file) {
    ReplayResult result;

    DFTRACER_UTILS_LOG_DEBUG("Starting replay of file: %s", trace_file.c_str());

    auto start_time = std::chrono::steady_clock::now();

    try {
        replay_file_async(this, trace_file, index_file, &result).get();
    } catch (const std::exception& e) {
        result.error_messages.push_back("Exception during replay: " +
                                        std::string(e.what()));
    }

    auto end_time = std::chrono::steady_clock::now();
    result.total_duration =
        std::chrono::duration_cast<std::chrono::microseconds>(end_time -
                                                              start_time);

    DFTRACER_UTILS_LOG_DEBUG(
        "Replay completed. Total events: %zu, Executed: %zu, Failed: %zu",
        result.total_events, result.executed_events, result.failed_events);

    return result;
}

ReplayResult ReplayEngine::replay(const std::vector<std::string>& trace_files) {
    ReplayResult aggregated_result;

    for (const auto& file : trace_files) {
        ReplayResult file_result = replay(file);

        // Aggregate results
        aggregated_result.total_events += file_result.total_events;
        aggregated_result.executed_events += file_result.executed_events;
        aggregated_result.filtered_events += file_result.filtered_events;
        aggregated_result.failed_events += file_result.failed_events;
        aggregated_result.total_duration += file_result.total_duration;
        aggregated_result.execution_duration += file_result.execution_duration;

        // Merge function and category counts
        for (const auto& [func, count] : file_result.function_counts) {
            aggregated_result.function_counts[func] += count;
        }
        for (const auto& [cat, count] : file_result.category_counts) {
            aggregated_result.category_counts[cat] += count;
        }

        // Merge error messages
        aggregated_result.error_messages.insert(
            aggregated_result.error_messages.end(),
            file_result.error_messages.begin(),
            file_result.error_messages.end());
    }

    return aggregated_result;
}

bool ReplayEngine::process_trace_line(common::json::JsonParser& parser,
                                      ReplayResult& result) {
    Trace trace;
    if (!parse_trace_json(parser, trace)) {
        return false;
    }
    dispatch_trace(trace, result);
    return true;
}

void ReplayEngine::dispatch_trace(const Trace& trace, ReplayResult& result) {
    result.total_events++;
    result.function_counts[trace.func_name]++;
    result.category_counts[trace.cat]++;
    result.pid_counts[static_cast<std::uint32_t>(trace.pid)]++;
    result.tid_counts[static_cast<std::uint32_t>(trace.tid)]++;

    // Track timestamp range
    if (trace.time_start > 0) {
        if (trace.time_start < result.first_timestamp) {
            result.first_timestamp = trace.time_start;
        }
        if (trace.time_start > result.last_timestamp) {
            result.last_timestamp = trace.time_start;
        }
    }

    // Track I/O bytes
    if (trace.size > 0) {
        if (trace.func_name.find("read") != std::string::npos ||
            trace.func_name.find("Read") != std::string::npos) {
            result.total_bytes_read += static_cast<std::size_t>(trace.size);
        } else if (trace.func_name.find("write") != std::string::npos ||
                   trace.func_name.find("Write") != std::string::npos) {
            result.total_bytes_written += static_cast<std::size_t>(trace.size);
        }
    }

    // Check max events limit
    if (config_.max_events > 0 &&
        result.executed_events >= config_.max_events) {
        // Silently skip - limit already reached
        return;
    }

    if (!should_execute_trace(trace)) {
        result.filtered_events++;
        return;
    }

    // Apply timing logic (skip during dry-run or dftracer-mode)
    if (config_.maintain_timing && !config_.dry_run && !config_.dftracer_mode &&
        trace.time_start > 0 && trace.type == TraceType::Regular) {
        apply_timing(trace);
    }

    // Fidelity-observation point: callers can hook here to capture the
    // wall-clock time at which each event is about to be dispatched and
    // compare it against the trace timeline. No production paths set this.
    if (config_.on_dispatch) {
        config_.on_dispatch(trace, std::chrono::steady_clock::now());
    }

    // Find and execute with appropriate executor
    TraceExecutor* executor = find_executor(trace);
    if (executor) {
        auto exec_start = std::chrono::steady_clock::now();
        bool success = executor->execute(trace, config_);
        auto exec_end = std::chrono::steady_clock::now();

        result.execution_duration +=
            std::chrono::duration_cast<std::chrono::microseconds>(exec_end -
                                                                  exec_start);

        if (success) {
            result.executed_events++;
        } else {
            result.failed_events++;
            std::string msg = "Failed to execute ";
            msg.append(trace.func_name);
            msg += " with ";
            msg += executor->get_name();
            result.error_messages.push_back(std::move(msg));
        }
    } else {
        result.failed_events++;
        DFTRACER_UTILS_LOG_DEBUG(
            "No executor found for function: %.*s (category: %.*s)",
            static_cast<int>(trace.func_name.size()), trace.func_name.data(),
            static_cast<int>(trace.cat.size()), trace.cat.data());
    }
}

bool ReplayEngine::parse_trace_json(common::json::JsonParser& parser,
                                    Trace& trace) {
    composites::dft::DFTracerEvent ev;
    // parse_ondemand returns false only when no "ph" was found; other fields
    // are still populated. Match the legacy DOM-based behavior, which keyed
    // validity on a non-empty name and treated missing ph as Regular.
    composites::dft::DFTracerEvent::parse_ondemand(parser, ev);

    if (ev.name.empty()) {
        return false;
    }

    trace.func_name = intern_sv(ev.name);
    trace.cat = intern_sv(ev.cat);
    trace.pid = ev.pid;
    trace.tid = ev.tid;
    trace.time_start = ev.ts;
    trace.duration = static_cast<double>(ev.dur);
    trace.time_end = trace.time_start + ev.dur;

    // ArgsValueProxy::get<string_view> returns a view directly into the
    // variant's owned string without copying; we then intern so the view
    // outlives ev/ArgsMap (which die at the end of this function).
    auto fhash_sv = ev.args["fhash"].get<std::string_view>(std::string_view{});
    auto hhash_sv = ev.args["hhash"].get<std::string_view>(std::string_view{});
    trace.fhash = fhash_sv.empty() ? std::string_view{} : intern_sv(fhash_sv);
    trace.hhash = hhash_sv.empty() ? std::string_view{} : intern_sv(hhash_sv);
    trace.size =
        ev.args["size"].get<std::int64_t>(static_cast<std::int64_t>(-1));
    trace.offset =
        ev.args["offset"].get<std::int64_t>(static_cast<std::int64_t>(-1));

    if (ev.ph == "M") {
        if (trace.func_name == "FH") {
            trace.type = TraceType::FileHash;
        } else if (trace.func_name == "HH") {
            trace.type = TraceType::HostHash;
        } else {
            trace.type = TraceType::OtherMetadata;
        }
    } else {
        trace.type = TraceType::Regular;
    }

    trace.is_valid = true;
    return true;
}

void ReplayEngine::apply_timing(const Trace& trace) {
    if (!config_.maintain_timing) {
        return;
    }

    if (!first_timestamp_set_) {
        // Anchor BOTH clocks on the first event. The wall-clock anchor was
        // initialized at engine construction time, but for any consumer
        // path with warmup (e.g. Pipeline producer fills, channel hops),
        // that anchor is "behind" by the warmup gap. Without resetting it
        // here, the next event sees replay_elapsed >> trace_elapsed and
        // we never sleep, collapsing the timing model. The trace-time
        // anchor is set on first event regardless, so co-locating the
        // wall-clock reset here keeps the two in lockstep.
        first_trace_timestamp_ = trace.time_start;
        replay_start_time_ = std::chrono::steady_clock::now();
        first_timestamp_set_ = true;
        return;
    }

    // Calculate elapsed time since first trace
    std::uint64_t trace_elapsed_us = trace.time_start - first_trace_timestamp_;

    // Apply timing scale
    std::uint64_t scaled_elapsed_us = static_cast<std::uint64_t>(
        static_cast<double>(trace_elapsed_us) * config_.timing_scale);

    // Calculate how long we should sleep
    auto replay_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - replay_start_time_);

    if (scaled_elapsed_us >
        static_cast<std::uint64_t>(replay_elapsed.count())) {
        std::uint64_t sleep_us =
            scaled_elapsed_us -
            static_cast<std::uint64_t>(replay_elapsed.count());

        const std::uint64_t MAX_SLEEP_US = 10 * 1000 * 1000;
        if (sleep_us > MAX_SLEEP_US) {
            DFTRACER_UTILS_LOG_DEBUG(
                "Warning: Capping sleep from %.3f ms to %.3f ms",
                static_cast<double>(sleep_us) / 1000.0,
                static_cast<double>(MAX_SLEEP_US) / 1000.0);
            sleep_us = MAX_SLEEP_US;
        }

        if (sleep_us > 1000) {
            DFTRACER_UTILS_LOG_DEBUG("Timing sleep: %.3f ms",
                                     static_cast<double>(sleep_us) / 1000.0);
        }

        std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
    }
}

bool ReplayEngine::should_execute_trace(const Trace& trace) const {
    // Check PID filters
    if (!config_.filter_pids.empty()) {
        if (config_.filter_pids.find(static_cast<std::uint32_t>(trace.pid)) ==
            config_.filter_pids.end()) {
            return false;
        }
    }
    if (!config_.exclude_pids.empty()) {
        if (config_.exclude_pids.find(static_cast<std::uint32_t>(trace.pid)) !=
            config_.exclude_pids.end()) {
            return false;
        }
    }

    // Check TID filters
    if (!config_.filter_tids.empty()) {
        if (config_.filter_tids.find(static_cast<std::uint32_t>(trace.tid)) ==
            config_.filter_tids.end()) {
            return false;
        }
    }
    if (!config_.exclude_tids.empty()) {
        if (config_.exclude_tids.find(static_cast<std::uint32_t>(trace.tid)) !=
            config_.exclude_tids.end()) {
            return false;
        }
    }

    // Check timestamp filters
    if (config_.start_timestamp > 0 &&
        trace.time_start < config_.start_timestamp) {
        return false;
    }
    if (config_.end_timestamp < UINT64_MAX &&
        trace.time_start > config_.end_timestamp) {
        return false;
    }

    // Check operation size filters
    if (config_.min_operation_size >= 0 && trace.size >= 0 &&
        trace.size < config_.min_operation_size) {
        return false;
    }
    if (config_.max_operation_size >= 0 && trace.size >= 0 &&
        trace.size > config_.max_operation_size) {
        return false;
    }

    if (!config_.filter_functions.empty()) {
        std::string key(trace.func_name);
        if (config_.filter_functions.find(key) ==
            config_.filter_functions.end()) {
            return false;
        }
    }
    if (!config_.exclude_functions.empty()) {
        std::string key(trace.func_name);
        if (config_.exclude_functions.find(key) !=
            config_.exclude_functions.end()) {
            return false;
        }
    }

    // Check category filters
    if (!config_.filter_categories.empty()) {
        std::string key(trace.cat);
        if (config_.filter_categories.find(key) ==
            config_.filter_categories.end()) {
            return false;
        }
    }
    if (!config_.exclude_categories.empty()) {
        std::string key(trace.cat);
        if (config_.exclude_categories.find(key) !=
            config_.exclude_categories.end()) {
            return false;
        }
    }

    // Apply sampling
    if (config_.sampling_rate < 1.0) {
        if (config_.sample_deterministic) {
            std::hash<std::uint64_t> hasher;
            std::size_t hash = hasher(trace.pid + trace.tid + trace.time_start +
                                      config_.sample_seed);
            double normalized = static_cast<double>(hash % 10000) / 10000.0;
            if (normalized >= config_.sampling_rate) {
                return false;
            }
        } else {
            static std::mt19937_64 rng(config_.sample_seed);
            static std::uniform_real_distribution<double> dist(0.0, 1.0);
            if (dist(rng) >= config_.sampling_rate) {
                return false;
            }
        }
    }

    // Skip metadata events by default
    if (trace.type != TraceType::Regular) {
        return false;
    }

    return true;
}

TraceExecutor* ReplayEngine::find_executor(const Trace& trace) {
    for (auto& executor : executors_) {
        if (executor->can_handle(trace)) {
            return executor.get();
        }
    }
    return nullptr;
}

std::string ReplayEngine::get_replay_file_path(
    const std::string& original_path) const {
    if (config_.output_directory.empty()) {
        return original_path;
    }

    std::size_t last_slash = original_path.find_last_of('/');
    std::string filename = (last_slash != std::string::npos)
                               ? original_path.substr(last_slash + 1)
                               : original_path;

    return config_.output_directory + "/" + filename;
}

// =============================================================================
// Call Tree Replay Implementation
// =============================================================================

ReplayResult ReplayEngine::replay_with_call_tree(
    const std::string& trace_directory, const std::string& pattern) {
    ReplayResult result;

    DFTRACER_UTILS_LOG_DEBUG("Starting call tree replay from directory: %s",
                             trace_directory.c_str());

    auto start_time = std::chrono::steady_clock::now();

    try {
        // Create CallTree instance using public API
        dftracer::utils::call_tree::CallTree call_tree;

        // Load trace files into call tree
        DFTRACER_UTILS_LOG_DEBUG("Loading trace files into call tree...");
        if (!call_tree.load_from_directory(trace_directory, pattern)) {
            result.error_messages.push_back(
                "Failed to load trace files from directory: " +
                trace_directory);
            return result;
        }

        // Generate the call tree structure
        DFTRACER_UTILS_LOG_DEBUG("Generating call tree structure...");
        if (!call_tree.generate()) {
            result.error_messages.push_back(
                "Failed to generate call tree structure");
            return result;
        }

        // Get statistics before replay
        auto stats = call_tree.get_statistics();
        result.total_nodes = stats.total_nodes;
        result.tree_depth = static_cast<std::size_t>(stats.max_depth);
        result.unique_processes = stats.unique_processes;

        DFTRACER_UTILS_LOG_DEBUG(
            "Call tree loaded: %zu nodes, depth %d, %zu processes",
            result.total_nodes, stats.max_depth, result.unique_processes);

        // Replay from the call tree
        if (config_.hierarchical_replay) {
            DFTRACER_UTILS_LOG_DEBUG("Performing hierarchical replay...");
            replay_from_call_tree(call_tree, result);
        } else {
            DFTRACER_UTILS_LOG_DEBUG(
                "Performing linear replay with call tree filtering...");
            auto nodes = call_tree.get_all_nodes();
            for (const auto& node : nodes) {
                replay_call_tree_node(node, result);
            }
        }

    } catch (const std::exception& e) {
        result.error_messages.push_back("Exception during call tree replay: " +
                                        std::string(e.what()));
    }

    auto end_time = std::chrono::steady_clock::now();
    result.total_duration =
        std::chrono::duration_cast<std::chrono::microseconds>(end_time -
                                                              start_time);

    DFTRACER_UTILS_LOG_DEBUG(
        "Call tree replay completed. Total nodes: %zu, Executed: %zu, Failed: "
        "%zu",
        result.total_nodes, result.executed_events, result.failed_events);

    return result;
}

void ReplayEngine::replay_from_call_tree(
    dftracer::utils::call_tree::CallTree& call_tree, ReplayResult& result) {
    // Get root nodes for each process
    auto processes = call_tree.get_process_ids();

    for (std::uint32_t pid : processes) {
        auto threads = call_tree.get_thread_ids(pid);

        for (std::uint32_t tid : threads) {
            auto root_nodes = call_tree.get_root_nodes(pid, tid);

            DFTRACER_UTILS_LOG_DEBUG(
                "Processing process %u, thread %u: %zu root nodes", pid, tid,
                root_nodes.size());

            for (const auto& root_node : root_nodes) {
                replay_call_tree_node_recursive(root_node, call_tree, result,
                                                0);
            }
        }
    }
}

void ReplayEngine::replay_call_tree_node_recursive(
    const dftracer::utils::call_tree::CallTreeNodeInfo& node,
    dftracer::utils::call_tree::CallTree& call_tree, ReplayResult& result,
    int depth) {
    // Apply depth limits if configured
    if (config_.max_level >= 0 && depth > config_.max_level) {
        result.filtered_events++;
        return;
    }
    if (config_.min_level >= 0 && depth < config_.min_level) {
        result.filtered_events++;
        return;
    }

    // Replay the current node
    replay_call_tree_node(node, result);

    // If respecting call hierarchy, replay children
    if (config_.respect_call_hierarchy) {
        for (std::uint64_t child_id : node.children_ids) {
            auto child_node = call_tree.get_node_by_id(child_id);
            if (child_node.id != 0) {
                replay_call_tree_node_recursive(child_node, call_tree, result,
                                                depth + 1);
            }
        }
    }
}

void ReplayEngine::replay_call_tree_node(
    const dftracer::utils::call_tree::CallTreeNodeInfo& node,
    ReplayResult& result) {
    // Convert CallTreeNodeInfo to Trace structure
    Trace trace;
    trace.func_name = intern_sv(node.name);
    trace.cat = intern_sv(node.category);
    trace.time_start = node.start_time_us;
    trace.duration = static_cast<double>(node.duration_us);
    trace.time_end = trace.time_start + node.duration_us;
    trace.type = TraceType::Regular;
    trace.is_valid = true;

    // Extract args from node
    const auto& args = node.args;

    auto pid_it = args.find("pid");
    if (pid_it != args.end()) {
        try {
            trace.pid = std::stoull(pid_it->second);
        } catch (...) {
            trace.pid = 0;
        }
    }

    auto tid_it = args.find("tid");
    if (tid_it != args.end()) {
        try {
            trace.tid = std::stoull(tid_it->second);
        } catch (...) {
            trace.tid = 0;
        }
    }

    auto fhash_it = args.find("fhash");
    if (fhash_it != args.end() && !fhash_it->second.empty()) {
        trace.fhash = intern_sv(fhash_it->second);
    }

    auto hhash_it = args.find("hhash");
    if (hhash_it != args.end() && !hhash_it->second.empty()) {
        trace.hhash = intern_sv(hhash_it->second);
    }

    auto size_it = args.find("size");
    if (size_it != args.end()) {
        try {
            trace.size = std::stoll(size_it->second);
        } catch (...) {
            trace.size = -1;
        }
    }

    auto offset_it = args.find("offset");
    if (offset_it != args.end()) {
        try {
            trace.offset = std::stoll(offset_it->second);
        } catch (...) {
            trace.offset = -1;
        }
    }

    // Update result statistics
    result.total_events++;
    result.function_counts[trace.func_name]++;
    result.category_counts[trace.cat]++;
    result.pid_counts[static_cast<std::uint32_t>(trace.pid)]++;
    result.tid_counts[static_cast<std::uint32_t>(trace.tid)]++;

    // Track timestamp range
    if (trace.time_start > 0) {
        if (trace.time_start < result.first_timestamp) {
            result.first_timestamp = trace.time_start;
        }
        if (trace.time_start > result.last_timestamp) {
            result.last_timestamp = trace.time_start;
        }
    }

    // Track I/O bytes
    if (trace.size > 0) {
        if (trace.func_name.find("read") != std::string::npos ||
            trace.func_name.find("Read") != std::string::npos) {
            result.total_bytes_read += static_cast<std::size_t>(trace.size);
        } else if (trace.func_name.find("write") != std::string::npos ||
                   trace.func_name.find("Write") != std::string::npos) {
            result.total_bytes_written += static_cast<std::size_t>(trace.size);
        }
    }

    // Check max events limit
    if (config_.max_events > 0 &&
        result.executed_events >= config_.max_events) {
        // Silently skip - limit already reached
        return;
    }

    // Apply filters
    if (!should_execute_trace(trace)) {
        result.filtered_events++;
        return;
    }

    // Apply timing logic
    if (config_.maintain_timing && !config_.dry_run && !config_.dftracer_mode &&
        trace.time_start > 0) {
        apply_timing(trace);
    }

    // Find and execute with appropriate executor
    TraceExecutor* executor = find_executor(trace);
    if (executor) {
        auto exec_start = std::chrono::steady_clock::now();
        bool success = executor->execute(trace, config_);
        auto exec_end = std::chrono::steady_clock::now();

        result.execution_duration +=
            std::chrono::duration_cast<std::chrono::microseconds>(exec_end -
                                                                  exec_start);

        if (success) {
            result.executed_events++;
        } else {
            result.failed_events++;
            std::string msg = "Failed to execute ";
            msg.append(trace.func_name);
            msg += " with ";
            msg += executor->get_name();
            result.error_messages.push_back(std::move(msg));
        }
    } else {
        result.failed_events++;
        DFTRACER_UTILS_LOG_DEBUG(
            "No executor found for function: %.*s (category: %.*s)",
            static_cast<int>(trace.func_name.size()), trace.func_name.data(),
            static_cast<int>(trace.cat.size()), trace.cat.data());
    }
}

}  // namespace dftracer::utils::utilities::replay
