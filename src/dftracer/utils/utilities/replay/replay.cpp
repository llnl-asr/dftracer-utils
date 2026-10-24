#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dataframe/containment.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/json/parser.h>
#include <dftracer/utils/runtime/coro/cancelled.h>
#include <dftracer/utils/runtime/coro/when_all.h>
#include <dftracer/utils/runtime/coro/when_any.h>
#include <dftracer/utils/runtime/executor/executor.h>
#include <dftracer/utils/runtime/executor/resumption_helper.h>
#include <dftracer/utils/trace/event.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <dftracer/utils/utilities/replay/replay.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <random>
#include <thread>
#include <unordered_set>
#include <utility>

namespace dftracer::utils::utilities::replay {

namespace {

// Process-wide intern pool for replay strings. Function names, categories,
// and per-file hashes (fhash/hhash) have either small bounded cardinality
// (~tens for cat/name) or stable identity per file (fhash).
dftracer::utils::dstd::StringIntern& replay_intern() {
    static dftracer::utils::dstd::StringIntern instance;
    return instance;
}

std::string_view intern_sv(std::string_view sv) {
    return replay_intern().intern(sv);
}

// A wait this long or longer suspends on the executor's timer; a shorter one
// sleeps the thread, since the timer is not that precise.
constexpr std::chrono::microseconds TIMER_WAIT_MIN{1000};

coro::CoroTask<void> wait_for(std::chrono::microseconds wait) {
    Executor* executor = resume_executor_for(nullptr);
    if (wait < TIMER_WAIT_MIN || executor == nullptr) {
        std::this_thread::sleep_for(wait);
        co_return;
    }
    try {
        co_await coro::timeout(wait, &executor->get_timer_service());
    } catch (const DFTUtilsException& e) {
        if (coro::is_cancelled(e)) throw;
    }
}

}  // anonymous namespace

// =============================================================================
// ReplayEngine Implementation
// =============================================================================

ReplayEngine::ReplayEngine(const ReplayConfig& config) : config_(config) {}

ReplayEngine::ReplayEngine(const std::string& /* trace_file */,
                           const ReplayConfig& config)
    : config_(config) {}

ReplayEngine::~ReplayEngine() = default;

ReplayEngine::Lane ReplayEngine::make_lane() const {
    Lane lane;
    if (config_.dftracer_mode) {
        lane.executor = std::make_unique<DFTracerExecutor>();
    } else {
        lane.executor = std::make_unique<PosixExecutor>();
    }
    return lane;
}

coro::AsyncGenerator<Trace> ReplayEngine::stream_traces(std::string file) {
    using reader::ReadConfig;
    using reader::TraceReader;
    using reader::TraceReaderConfig;

    TraceReaderConfig cfg;
    cfg.file_path = std::move(file);
    // Replay streams every event in order, so it needs no index; skip the
    // auto-build to stay on the plain sequential read path.
    cfg.auto_build_index = false;
    TraceReader rdr(std::move(cfg));
    auto gen = rdr.read_json(ReadConfig{});
    for (;;) {
        auto opt = co_await gen.next();
        if (!opt) break;
        if (!opt->parser) continue;
        Trace trace;
        if (parse_trace_json(*opt->parser, trace)) {
            co_yield std::move(trace);
        }
    }
}

coro::CoroTask<void> ReplayEngine::replay_file(std::string file, Lane& lane,
                                               std::size_t channel_capacity) {
    coro::Channel<Trace> ch_instance(channel_capacity);
    auto* channel = &ch_instance;

    // Run producer and consumer concurrently via when_all rather than spawning
    // them inside a nested scope: the extra spawn-and-return coroutine layer
    // let the scope report completion before the consumer had drained the
    // channel, dropping every buffered event under load.
    auto producer = [this, channel, &file]() -> coro::CoroTask<void> {
        auto p = channel->producer();
        auto guard = p.guard();
        auto gen = stream_traces(file);
        for (;;) {
            auto trace = co_await gen.next();
            if (!trace) break;
            const auto send_ok = co_await p.send(std::move(*trace));
            if (!send_ok) {
                co_return;
            }
        }
        co_return;
    };
    auto consumer = [this, channel, &lane]() -> coro::CoroTask<void> {
        auto c = channel->consumer();
        for (;;) {
            auto item = co_await c.receive();
            if (!item) break;
            std::chrono::microseconds wait{0};
            if (!admit(*item, lane, wait)) continue;
            if (wait.count() > 0) co_await wait_for(wait);
            execute(*item, lane);
        }
        co_return;
    };

    co_await coro::when_all(producer(), consumer());

    co_return;
}

coro::CoroTask<void> ReplayEngine::run_pipelined(
    dftracer::utils::CoroScope& scope, const std::vector<std::string>& files,
    ReplayResult& result, std::size_t channel_capacity) {
    admitted_.store(0, std::memory_order_relaxed);
    std::vector<Lane> lanes(files.size());
    Executor* executor = resume_executor_for(nullptr);
    const std::size_t threads =
        executor != nullptr ? executor->get_num_threads() : 1;
    const std::size_t workers =
        std::min(files.size(), std::max<std::size_t>(threads, 1));
    std::atomic<std::size_t> next{0};

    std::vector<coro::SpawnFuture<void>> runs;
    runs.reserve(workers);
    for (std::size_t w = 0; w < workers; ++w) {
        runs.push_back(
            scope.spawn([this, &files, &lanes, &next,
                         channel_capacity](CoroScope&) -> coro::CoroTask<void> {
                for (std::size_t i = next.fetch_add(1); i < files.size();
                     i = next.fetch_add(1)) {
                    lanes[i] = make_lane();
                    co_await replay_file(files[i], lanes[i], channel_capacity);
                }
            }));
    }
    co_await coro::when_all(std::move(runs));

    for (const Lane& lane : lanes) result.merge(lane.result);
    co_return;
}

namespace {

// Sync drive used by the existing replay(file)/replay(vector) entry points.
// Pipeline-driven callers use ReplayEngine::run_pipelined instead.
template <typename OnEvent>
coro::CoroTask<void> read_file_events(std::string trace_file,
                                      std::string index_file,
                                      OnEvent on_event) {
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
    for (;;) {
        auto opt = co_await gen.next();
        if (!opt) break;
        if (!opt->parser) continue;
        on_event(*opt->parser);
    }
    co_return;
}

}  // namespace

ReplayResult ReplayEngine::replay(const std::string& trace_file,
                                  const std::string& index_file) {
    admitted_.store(0, std::memory_order_relaxed);
    return replay_file_sync(trace_file, index_file);
}

ReplayResult ReplayEngine::replay_file_sync(const std::string& trace_file,
                                            const std::string& index_file) {
    Lane lane = make_lane();

    DFTRACER_UTILS_LOG_DEBUG("Starting replay of file: %s", trace_file.c_str());

    auto start_time = std::chrono::steady_clock::now();

    try {
        read_file_events(trace_file, index_file,
                         [this, &lane](json::JsonParser& parser) {
                             Trace trace;
                             if (parse_trace_json(parser, trace))
                                 replay_one_trace(trace, lane);
                         })
            .get();
    } catch (const std::exception& e) {
        lane.result.error_messages.push_back("Exception during replay: " +
                                             std::string(e.what()));
    }
    ReplayResult result = std::move(lane.result);

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

    admitted_.store(0, std::memory_order_relaxed);
    for (const auto& file : trace_files) {
        aggregated_result.merge(replay_file_sync(file, ""));
    }

    return aggregated_result;
}

bool ReplayEngine::admit(const Trace& trace, Lane& lane,
                         std::chrono::microseconds& wait) {
    ReplayResult& result = lane.result;
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

    // Silently skip once the limit is reached.
    if (config_.max_events > 0 &&
        admitted_.load(std::memory_order_relaxed) >= config_.max_events) {
        return false;
    }

    if (!should_execute_trace(trace)) {
        result.filtered_events++;
        return false;
    }

    if (config_.max_events > 0 &&
        admitted_.fetch_add(1, std::memory_order_relaxed) >=
            config_.max_events) {
        return false;
    }

    // Timing applies only to a real replay, not to dry-run or dftracer-mode.
    wait = std::chrono::microseconds{0};
    if (config_.maintain_timing && !config_.dry_run && !config_.dftracer_mode &&
        trace.time_start > 0 && trace.type == TraceType::Regular) {
        wait = timing_wait(trace, lane);
    }
    return true;
}

void ReplayEngine::execute(const Trace& trace, Lane& lane) {
    ReplayResult& result = lane.result;

    // Fidelity-observation point: callers can hook here to capture the
    // wall-clock time at which each event is about to be dispatched and
    // compare it against the trace timeline. No production paths set this.
    if (config_.on_dispatch) {
        config_.on_dispatch(trace, std::chrono::steady_clock::now());
    }

    TraceExecutor* executor = lane.executor && lane.executor->can_handle(trace)
                                  ? lane.executor.get()
                                  : nullptr;
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

bool ReplayEngine::parse_trace_json(json::JsonParser& parser, Trace& trace) {
    trace::DFTracerEvent ev;
    // parse_ondemand returns false only when no "ph" was found; other fields
    // are still populated. Match the legacy DOM-based behavior, which keyed
    // validity on a non-empty name and treated missing ph as Regular.
    trace::DFTracerEvent::parse_ondemand(parser, ev);

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

    if (ev.is_metadata()) {
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

void ReplayEngine::replay_one_trace(const Trace& trace, Lane& lane) {
    std::chrono::microseconds wait{0};
    if (!admit(trace, lane, wait)) return;
    if (wait.count() > 0) std::this_thread::sleep_for(wait);
    execute(trace, lane);
}

std::chrono::microseconds ReplayEngine::timing_wait(const Trace& trace,
                                                    Lane& lane) const {
    if (!lane.anchored) {
        // Anchor BOTH clocks on the first event. The wall-clock anchor was
        // initialized at engine construction time, but for any consumer
        // path with warmup (e.g. Pipeline producer fills, channel hops),
        // that anchor is "behind" by the warmup gap. Without resetting it
        // here, the next event sees replay_elapsed >> trace_elapsed and
        // we never sleep, collapsing the timing model. The trace-time
        // anchor is set on first event regardless, so co-locating the
        // wall-clock reset here keeps the two in lockstep.
        lane.first_timestamp = trace.time_start;
        lane.start = std::chrono::steady_clock::now();
        lane.anchored = true;
        return std::chrono::microseconds{0};
    }

    // An event older than the anchor runs at once.
    if (trace.time_start <= lane.first_timestamp) {
        return std::chrono::microseconds{0};
    }
    std::uint64_t trace_elapsed_us = trace.time_start - lane.first_timestamp;

    // Apply timing scale
    std::uint64_t scaled_elapsed_us = static_cast<std::uint64_t>(
        static_cast<double>(trace_elapsed_us) * config_.timing_scale);

    // Calculate how long we should sleep
    auto replay_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - lane.start);

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

        return std::chrono::microseconds(sleep_us);
    }
    return std::chrono::microseconds{0};
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
            thread_local std::mt19937_64 rng(config_.sample_seed);
            thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);
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

// =============================================================================
// Call Tree Replay: containment (level/parent) computed locally over the
// events, so replay keeps every Trace field and needs no separate call-tree
// subsystem.
// =============================================================================

namespace {
// Files under `dir` whose name ends with `pattern`'s suffix (e.g. "*.pfw.gz").
std::vector<std::string> glob_trace_files(const std::string& dir,
                                          const std::string& pattern) {
    std::string suffix = pattern;
    if (auto star = suffix.find('*'); star != std::string::npos)
        suffix = suffix.substr(star + 1);
    std::vector<std::string> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        const std::string s = it->path().string();
        if (suffix.empty() ||
            (s.size() >= suffix.size() &&
             s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0))
            out.push_back(s);
    }
    std::sort(out.begin(), out.end());
    return out;
}
}  // namespace

ReplayResult ReplayEngine::replay_with_call_tree(
    const std::string& trace_directory, const std::string& pattern) {
    admitted_.store(0, std::memory_order_relaxed);
    Lane lane = make_lane();
    ReplayResult& result = lane.result;
    auto start_time = std::chrono::steady_clock::now();
    try {
        std::vector<std::string> files =
            glob_trace_files(trace_directory, pattern);
        if (files.empty()) {
            result.error_messages.push_back("No trace files in directory: " +
                                            trace_directory);
            return std::move(lane.result);
        }

        // Read every event once.
        std::vector<Trace> events;
        [&]() -> coro::CoroTask<void> {
            for (const std::string& file : files) {
                auto gen = stream_traces(file);
                for (;;) {
                    auto opt = co_await gen.next();
                    if (!opt) break;
                    events.push_back(std::move(*opt));
                }
            }
            co_return;
        }()
                     .get();

        const std::int64_t n = static_cast<std::int64_t>(events.size());
        auto start = [&](std::int64_t i) {
            return static_cast<std::int64_t>(
                events[static_cast<std::size_t>(i)].time_start);
        };
        auto end = [&](std::int64_t i) {
            const Trace& t = events[static_cast<std::size_t>(i)];
            return static_cast<std::int64_t>(
                t.time_start + static_cast<std::uint64_t>(t.duration));
        };

        // Lanes by (pid,tid); each is an independent nesting stack.
        // containment_ walk assigns level (depth) and parent (enclosing event's
        // index).
        std::map<std::pair<std::uint64_t, std::uint64_t>,
                 std::vector<std::int64_t>>
            lane_map;
        for (std::int64_t i = 0; i < n; ++i)
            lane_map[{events[static_cast<std::size_t>(i)].pid,
                      events[static_cast<std::size_t>(i)].tid}]
                .push_back(i);
        std::vector<std::int64_t> level(static_cast<std::size_t>(n), 0);
        std::vector<std::int64_t> parent(static_cast<std::size_t>(n), -1);
        for (auto& kv : lane_map) {
            auto& rows = kv.second;
            std::stable_sort(rows.begin(), rows.end(),
                             [&](std::int64_t a, std::int64_t b) {
                                 if (start(a) != start(b))
                                     return start(a) < start(b);
                                 return end(a) > end(b);
                             });
            std::vector<std::pair<std::int64_t, std::int64_t>> stack;
            dataframe::containment_walk(
                static_cast<std::int64_t>(rows.size()),
                [&](std::int64_t k) { return start(rows[k]); },
                [&](std::int64_t k) { return end(rows[k]); }, std::int64_t(-1),
                [&](std::int64_t k, std::int64_t lv,
                    std::int64_t par) -> std::int64_t {
                    const std::int64_t r = rows[k];
                    level[static_cast<std::size_t>(r)] = lv;
                    parent[static_cast<std::size_t>(r)] = par;
                    return r;
                },
                stack);
        }

        result.total_nodes = static_cast<std::size_t>(n);
        std::int64_t max_level = 0;
        std::unordered_set<std::uint64_t> pids;
        for (std::int64_t i = 0; i < n; ++i) {
            max_level = std::max(max_level, level[static_cast<std::size_t>(i)]);
            pids.insert(events[static_cast<std::size_t>(i)].pid);
        }
        result.tree_depth = static_cast<std::size_t>(max_level);
        result.unique_processes = pids.size();

        if (config_.hierarchical_replay) {
            std::vector<std::vector<std::int64_t>> children(
                static_cast<std::size_t>(n));
            std::vector<std::int64_t> roots;
            for (std::int64_t i = 0; i < n; ++i) {
                if (parent[static_cast<std::size_t>(i)] < 0)
                    roots.push_back(i);
                else
                    children[static_cast<std::size_t>(
                                 parent[static_cast<std::size_t>(i)])]
                        .push_back(i);
            }
            auto by_start = [&](std::int64_t a, std::int64_t b) {
                return start(a) < start(b);
            };
            std::stable_sort(roots.begin(), roots.end(), by_start);
            for (auto& ch : children)
                std::stable_sort(ch.begin(), ch.end(), by_start);
            std::function<void(std::int64_t, int)> dfs = [&](std::int64_t idx,
                                                             int depth) {
                if (config_.max_level >= 0 && depth > config_.max_level) {
                    result.filtered_events++;
                    return;
                }
                if (config_.min_level >= 0 && depth < config_.min_level) {
                    result.filtered_events++;
                    return;
                }
                replay_one_trace(events[static_cast<std::size_t>(idx)], lane);
                if (config_.respect_call_hierarchy)
                    for (std::int64_t c :
                         children[static_cast<std::size_t>(idx)])
                        dfs(c, depth + 1);
            };
            for (std::int64_t r : roots) dfs(r, 0);
        } else {
            for (std::int64_t i = 0; i < n; ++i)
                replay_one_trace(events[static_cast<std::size_t>(i)], lane);
        }
    } catch (const std::exception& e) {
        result.error_messages.push_back("Exception during call tree replay: " +
                                        std::string(e.what()));
    }
    auto end_time = std::chrono::steady_clock::now();
    result.total_duration =
        std::chrono::duration_cast<std::chrono::microseconds>(end_time -
                                                              start_time);
    return std::move(lane.result);
}

}  // namespace dftracer::utils::utilities::replay
