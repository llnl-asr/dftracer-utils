#include <dftracer/utils/call_tree/call_tree.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <dftracer/utils/utilities/reader/internal/reader_factory.h>
#include <dftracer/utils/utilities/replay/replay.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yyjson.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <thread>

namespace dftracer::utils::utilities::replay {

namespace {

/**
 * Trim whitespace and validate JSON string
 */
bool json_trim_and_validate(const char* input, std::size_t input_length,
                            const char*& trimmed, std::size_t& trimmed_length) {
    if (!input || input_length == 0) {
        return false;
    }

    // Trim leading whitespace
    std::size_t start = 0;
    while (start < input_length &&
           (input[start] == ' ' || input[start] == '\t' ||
            input[start] == '\n' || input[start] == '\r')) {
        start++;
    }

    // Trim trailing whitespace and comma
    std::size_t end = input_length;
    while (end > start && (input[end - 1] == ' ' || input[end - 1] == '\t' ||
                           input[end - 1] == '\n' || input[end - 1] == '\r' ||
                           input[end - 1] == ',')) {
        end--;
    }

    if (start >= end) {
        return false;
    }

    trimmed = input + start;
    trimmed_length = end - start;

    // Basic validation: must start with '{' and end with '}'
    if (trimmed[0] != '{' || trimmed[trimmed_length - 1] != '}') {
        return false;
    }

    return true;
}

/**
 * Parse a JSON string value from yyjson
 */
std::string get_json_string(yyjson_val* val, const char* key,
                            const std::string& default_value = "") {
    yyjson_val* field = yyjson_obj_get(val, key);
    if (field && yyjson_is_str(field)) {
        return yyjson_get_str(field);
    }
    return default_value;
}

/**
 * Parse a JSON uint64 value from yyjson
 */
std::uint64_t get_json_uint64(yyjson_val* val, const char* key,
                              std::uint64_t default_value = 0) {
    yyjson_val* field = yyjson_obj_get(val, key);
    if (field && yyjson_is_uint(field)) {
        return yyjson_get_uint(field);
    } else if (field && yyjson_is_int(field)) {
        std::int64_t int_val = yyjson_get_int(field);
        return int_val >= 0 ? static_cast<std::uint64_t>(int_val)
                            : default_value;
    }
    return default_value;
}

/**
 * Parse a JSON double value from yyjson
 */
double get_json_double(yyjson_val* val, const char* key,
                       double default_value = 0.0) {
    yyjson_val* field = yyjson_obj_get(val, key);
    if (field && yyjson_is_real(field)) {
        return yyjson_get_real(field);
    } else if (field && yyjson_is_uint(field)) {
        return static_cast<double>(yyjson_get_uint(field));
    } else if (field && yyjson_is_int(field)) {
        return static_cast<double>(yyjson_get_int(field));
    }
    return default_value;
}

/**
 * Get a string value from args object
 */
std::string get_args_string(yyjson_val* root, const char* key,
                            const std::string& default_value = "") {
    yyjson_val* args = yyjson_obj_get(root, "args");
    if (args && yyjson_is_obj(args)) {
        return get_json_string(args, key, default_value);
    }
    return default_value;
}

/**
 * Get an int64 value from args object
 */
std::int64_t get_args_int64(yyjson_val* root, const char* key,
                            std::int64_t default_value = 0) {
    yyjson_val* args = yyjson_obj_get(root, "args");
    if (args && yyjson_is_obj(args)) {
        yyjson_val* field = yyjson_obj_get(args, key);
        if (field && yyjson_is_int(field)) {
            return yyjson_get_int(field);
        } else if (field && yyjson_is_uint(field)) {
            return static_cast<std::int64_t>(yyjson_get_uint(field));
        }
    }
    return default_value;
}

/**
 * Create directory path if it doesn't exist
 */
bool ensure_directory_exists(const std::string& path) {
    std::string dir_path = path.substr(0, path.find_last_of('/'));
    if (dir_path.empty() || dir_path == path) return true;

    struct stat st;
    if (stat(dir_path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    // Try to create directory recursively
    return ensure_directory_exists(dir_path) &&
           (mkdir(dir_path.c_str(), 0755) == 0);
}

}  // anonymous namespace

// =============================================================================
// PosixExecutor Implementation
// =============================================================================

bool PosixExecutor::execute(const Trace& trace, const ReplayConfig& config) {
    const std::string& func_name = trace.func_name;

    if (config.dry_run) {
        DFTRACER_UTILS_LOG_DEBUG("DRY RUN: Would execute POSIX %s",
                                 func_name.c_str());
        return true;
    }

    if (func_name == "open" || func_name == "open64" || func_name == "openat") {
        return execute_open(trace, config);
    } else if (func_name == "close") {
        return execute_close(trace, config);
    } else if (func_name == "read" || func_name == "pread" ||
               func_name == "pread64") {
        return execute_read(trace, config);
    } else if (func_name == "write" || func_name == "pwrite" ||
               func_name == "pwrite64") {
        return execute_write(trace, config);
    } else if (func_name == "lseek" || func_name == "lseek64") {
        return execute_seek(trace, config);
    } else if (func_name == "stat" || func_name == "stat64" ||
               func_name == "lstat" || func_name == "fstat") {
        return execute_stat(trace, config);
    }

    DFTRACER_UTILS_LOG_DEBUG("Unsupported POSIX function: %s",
                             func_name.c_str());
    return false;
}

bool PosixExecutor::can_handle(const Trace& trace) const {
    return trace.cat == "posix" || trace.cat == "POSIX";
}

bool PosixExecutor::execute_open(const Trace& trace,
                                 const ReplayConfig& config) {
    DFTRACER_UTILS_LOG_DEBUG("Executing POSIX open");

    if (!trace.fhash.empty()) {
        std::string file_path =
            config.output_directory.empty()
                ? ("replay_file_" + trace.fhash)
                : (config.output_directory + "/replay_file_" + trace.fhash);

        ensure_directory_exists(file_path);

        int fd = open(file_path.c_str(), O_CREAT | O_RDWR, 0644);
        if (fd >= 0) {
            open_files_[trace.fhash] = fd;
            DFTRACER_UTILS_LOG_DEBUG("Opened file %s with fd %d",
                                     file_path.c_str(), fd);
            return true;
        } else {
            DFTRACER_UTILS_LOG_ERROR("Failed to open file %s: %s",
                                     file_path.c_str(), strerror(errno));
            return false;
        }
    }

    return true;
}

bool PosixExecutor::execute_close(const Trace& trace,
                                  [[maybe_unused]] const ReplayConfig& config) {
    DFTRACER_UTILS_LOG_DEBUG("Executing POSIX close");

    auto it = open_files_.find(trace.fhash);
    if (it != open_files_.end()) {
        close(it->second);
        open_files_.erase(it);
        DFTRACER_UTILS_LOG_DEBUG("Closed file with hash %s",
                                 trace.fhash.c_str());
    }

    return true;
}

bool PosixExecutor::execute_read(const Trace& trace,
                                 const ReplayConfig& config) {
    DFTRACER_UTILS_LOG_DEBUG("Executing POSIX read (size: %lld)",
                             static_cast<long long>(trace.size));

    auto it = open_files_.find(trace.fhash);
    if (it != open_files_.end() && trace.size > 0) {
        std::vector<char> buffer(std::min(static_cast<std::size_t>(trace.size),
                                          config.max_file_size));
        ssize_t bytes_read = read(it->second, buffer.data(), buffer.size());
        DFTRACER_UTILS_LOG_DEBUG("Read %zd bytes", bytes_read);
    }

    return true;
}

bool PosixExecutor::execute_write(const Trace& trace,
                                  const ReplayConfig& config) {
    DFTRACER_UTILS_LOG_DEBUG("Executing POSIX write (size: %lld)",
                             static_cast<long long>(trace.size));

    auto it = open_files_.find(trace.fhash);
    if (it != open_files_.end() && trace.size > 0) {
        std::size_t write_size = std::min(static_cast<std::size_t>(trace.size),
                                          config.max_file_size);
        std::vector<char> buffer(write_size, 'A');
        ssize_t bytes_written = write(it->second, buffer.data(), buffer.size());
        DFTRACER_UTILS_LOG_DEBUG("Wrote %zd bytes", bytes_written);
    }

    return true;
}

bool PosixExecutor::execute_seek(const Trace& trace,
                                 [[maybe_unused]] const ReplayConfig& config) {
    DFTRACER_UTILS_LOG_DEBUG("Executing POSIX seek (offset: %lld)",
                             static_cast<long long>(trace.offset));

    auto it = open_files_.find(trace.fhash);
    if (it != open_files_.end() && trace.offset >= 0) {
        off_t result = lseek(it->second, trace.offset, SEEK_SET);
        DFTRACER_UTILS_LOG_DEBUG("Seek to offset %lld, result: %lld",
                                 static_cast<long long>(trace.offset),
                                 static_cast<long long>(result));
    }

    return true;
}

bool PosixExecutor::execute_stat([[maybe_unused]] const Trace& trace,
                                 [[maybe_unused]] const ReplayConfig& config) {
    DFTRACER_UTILS_LOG_DEBUG("Executing POSIX stat");

    if (!trace.fhash.empty()) {
        DFTRACER_UTILS_LOG_DEBUG("Would stat file with hash %s",
                                 trace.fhash.c_str());
    }

    return true;
}

// =============================================================================
// DFTracerExecutor Implementation
// =============================================================================

bool DFTracerExecutor::execute(const Trace& trace, const ReplayConfig& config) {
    if (config.dry_run) {
        return true;
    }

    // Sleep for the duration of the operation instead of doing actual I/O
    double duration_us = static_cast<double>(trace.time_end - trace.time_start);

    // Cap individual operation sleeps to 1ms for practical testing
    const double MAX_DFTRACER_SLEEP_US = 1.0 * 1000.0;
    if (duration_us > MAX_DFTRACER_SLEEP_US) {
        duration_us = MAX_DFTRACER_SLEEP_US;
    }

    if (config.no_sleep) {
        if (config.verbose && duration_us >= 100000.0) {
            std::cout << "DFTracer would sleep for " << std::fixed
                      << std::setprecision(3) << duration_us / 1000.0
                      << " ms for " << trace.func_name << " (skipped)"
                      << std::endl;
        }
    } else {
        if (config.verbose && duration_us >= 100.0) {
            std::cout << "DFTracer sleeping for " << std::fixed
                      << std::setprecision(3) << duration_us / 1000.0
                      << " ms for " << trace.func_name << std::endl;
        }
        sleep_for_duration(duration_us);
    }

    return true;
}

bool DFTracerExecutor::can_handle([[maybe_unused]] const Trace& trace) const {
    return true;  // Handle all trace events
}

void DFTracerExecutor::sleep_for_duration(double duration_microseconds) {
    if (duration_microseconds <= 0) return;

    const double MAX_SLEEP_US = 10.0 * 1000.0 * 1000.0;
    if (duration_microseconds > MAX_SLEEP_US) {
        duration_microseconds = MAX_SLEEP_US;
    }

    auto sleep_duration = std::chrono::nanoseconds(
        static_cast<std::int64_t>(duration_microseconds * 1000));
    std::this_thread::sleep_for(sleep_duration);
}

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

ReplayResult ReplayEngine::replay(const std::string& trace_file,
                                  const std::string& index_file) {
    ReplayResult result;

    DFTRACER_UTILS_LOG_DEBUG("Starting replay of file: %s", trace_file.c_str());

    auto start_time = std::chrono::steady_clock::now();

    try {
        // Check if the file is compressed
        bool is_compressed =
            (trace_file.size() >= 3 &&
             trace_file.substr(trace_file.size() - 3) == ".gz") ||
            (trace_file.size() >= 7 &&
             trace_file.substr(trace_file.size() - 7) == ".tar.gz");

        if (is_compressed) {
            // Handle compressed files with ReaderFactory
            std::string idx_path =
                index_file.empty() ? utilities::composites::dft::internal::
                                         determine_index_path(trace_file, "")
                                   : index_file;

            auto reader =
                reader::internal::ReaderFactory::create(trace_file, idx_path);

            if (!reader) {
                result.error_messages.push_back(
                    "Failed to create reader for file: " + trace_file);
                return result;
            }

            // Create line processor for handling trace lines
            ReplayLineProcessor processor(*this, result);

            // Read all lines using the line processor
            reader->read_lines_with_processor(0, reader->get_num_lines(),
                                              processor);
        } else {
            // Handle plain text files directly
            std::ifstream file(trace_file);
            if (!file.is_open()) {
                result.error_messages.push_back(
                    "Failed to open plain text file: " + trace_file);
                return result;
            }

            std::string line;
            while (std::getline(file, line)) {
                // Skip empty lines and bracket lines
                if (line.empty() || line == "[" || line == "]") {
                    continue;
                }

                // Remove trailing comma if present
                if (!line.empty() && line.back() == ',') {
                    line.pop_back();
                }

                process_trace_line(line, result);
            }
        }

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

bool ReplayEngine::process_trace_line(const std::string& line,
                                      ReplayResult& result) {
    Trace trace;

    if (!parse_trace_json(line, trace)) {
        return false;
    }

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
        return false;
    }

    if (!should_execute_trace(trace)) {
        result.filtered_events++;
        return true;
    }

    // Apply timing logic (skip during dry-run or dftracer-mode)
    if (config_.maintain_timing && !config_.dry_run && !config_.dftracer_mode &&
        trace.time_start > 0 && trace.type == TraceType::Regular) {
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
            result.error_messages.push_back("Failed to execute " +
                                            trace.func_name + " with " +
                                            executor->get_name());
        }
    } else {
        result.failed_events++;
        if (config_.verbose) {
            DFTRACER_UTILS_LOG_DEBUG(
                "No executor found for function: %s (category: %s)",
                trace.func_name.c_str(), trace.cat.c_str());
        }
    }

    return true;
}

bool ReplayEngine::parse_trace_json(const std::string& json_line,
                                    Trace& trace) {
    const char* trimmed;
    std::size_t trimmed_length;
    if (!json_trim_and_validate(json_line.c_str(), json_line.length(), trimmed,
                                trimmed_length)) {
        return false;
    }

    yyjson_doc* doc = yyjson_read(trimmed, trimmed_length, 0);
    if (!doc) {
        return false;
    }

    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return false;
    }

    // Parse basic fields
    trace.func_name = get_json_string(root, "name");
    trace.cat = get_json_string(root, "cat");
    std::string phase = get_json_string(root, "ph");

    trace.pid = get_json_uint64(root, "pid");
    trace.tid = get_json_uint64(root, "tid");
    trace.time_start = get_json_uint64(root, "ts");
    trace.duration = get_json_double(root, "dur");
    trace.time_end =
        trace.time_start + static_cast<std::uint64_t>(trace.duration);

    // Parse arguments
    trace.fhash = get_args_string(root, "fhash");
    trace.hhash = get_args_string(root, "hhash");
    trace.size = get_args_int64(root, "size", -1);
    trace.offset = get_args_int64(root, "offset", -1);

    // Determine trace type
    if (phase == "M") {
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

    trace.is_valid = !trace.func_name.empty();

    yyjson_doc_free(doc);
    return trace.is_valid;
}

void ReplayEngine::apply_timing(const Trace& trace) {
    if (!config_.maintain_timing) {
        return;
    }

    if (!first_timestamp_set_) {
        first_trace_timestamp_ = trace.time_start;
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
            if (config_.verbose) {
                std::cout << "Warning: Capping sleep from "
                          << static_cast<double>(sleep_us) / 1000.0 << " ms to "
                          << MAX_SLEEP_US / 1000.0 << " ms" << std::endl;
            }
            sleep_us = MAX_SLEEP_US;
        }

        if (config_.verbose && sleep_us > 1000) {
            std::cout << "Timing sleep: "
                      << static_cast<double>(sleep_us) / 1000.0 << " ms"
                      << std::endl;
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

    // Check function filters
    if (!config_.filter_functions.empty()) {
        if (config_.filter_functions.find(trace.func_name) ==
            config_.filter_functions.end()) {
            return false;
        }
    }
    if (!config_.exclude_functions.empty()) {
        if (config_.exclude_functions.find(trace.func_name) !=
            config_.exclude_functions.end()) {
            return false;
        }
    }

    // Check category filters
    if (!config_.filter_categories.empty()) {
        if (config_.filter_categories.find(trace.cat) ==
            config_.filter_categories.end()) {
            return false;
        }
    }
    if (!config_.exclude_categories.empty()) {
        if (config_.exclude_categories.find(trace.cat) !=
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
    trace.func_name = node.name;
    trace.cat = node.category;
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
    if (fhash_it != args.end()) {
        trace.fhash = fhash_it->second;
    }

    auto hhash_it = args.find("hhash");
    if (hhash_it != args.end()) {
        trace.hhash = hhash_it->second;
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
            result.error_messages.push_back("Failed to execute " +
                                            trace.func_name + " with " +
                                            executor->get_name());
        }
    } else {
        result.failed_events++;
        if (config_.verbose) {
            DFTRACER_UTILS_LOG_DEBUG(
                "No executor found for function: %s (category: %s)",
                trace.func_name.c_str(), trace.cat.c_str());
        }
    }
}

// =============================================================================
// ReplayLineProcessor Implementation
// =============================================================================

ReplayLineProcessor::ReplayLineProcessor(ReplayEngine& engine,
                                         ReplayResult& result)
    : engine_(engine), result_(result) {}

coro::CoroTask<bool> ReplayLineProcessor::process(const char* data,
                                                  std::size_t length) {
    std::string line(data, length);
    co_return engine_.process_trace_line(line, result_);
}

// =============================================================================
// ReplayResult::print_summary Implementation
// =============================================================================

void ReplayResult::print_summary(bool verbose) const {
    std::cout << "\n=== Replay Summary ===" << std::endl;
    std::cout << "Total events: " << total_events << std::endl;
    std::cout << "Executed: " << executed_events << std::endl;
    std::cout << "Filtered: " << filtered_events << std::endl;
    std::cout << "Failed: " << failed_events << std::endl;

    double success_rate = total_events > 0
                              ? (static_cast<double>(executed_events) /
                                 static_cast<double>(total_events) * 100.0)
                              : 0.0;
    std::cout << "Success rate: " << std::fixed << std::setprecision(2)
              << success_rate << "%" << std::endl;

    std::cout << "\nTiming:" << std::endl;
    std::cout << "  Total duration: "
              << static_cast<double>(total_duration.count()) / 1000.0 << " ms"
              << std::endl;
    std::cout << "  Execution duration: "
              << static_cast<double>(execution_duration.count()) / 1000.0
              << " ms" << std::endl;

    if (first_timestamp != UINT64_MAX && last_timestamp > 0) {
        std::cout << "  Trace timespan: "
                  << static_cast<double>(last_timestamp - first_timestamp) /
                         1000000.0
                  << " seconds" << std::endl;
    }

    std::cout << "\nI/O Statistics:" << std::endl;
    std::cout << "  Bytes read: " << total_bytes_read << " ("
              << static_cast<double>(total_bytes_read) / (1024.0 * 1024.0)
              << " MB)" << std::endl;
    std::cout << "  Bytes written: " << total_bytes_written << " ("
              << static_cast<double>(total_bytes_written) / (1024.0 * 1024.0)
              << " MB)" << std::endl;

    std::cout << "\nProcess/Thread Statistics:" << std::endl;
    std::cout << "  Unique PIDs: " << pid_counts.size() << std::endl;
    std::cout << "  Unique TIDs: " << tid_counts.size() << std::endl;

    if (verbose) {
        if (!pid_counts.empty()) {
            std::cout << "\n  Events per PID:" << std::endl;
            for (const auto& [pid, count] : pid_counts) {
                std::cout << "    PID " << pid << ": " << count << " events"
                          << std::endl;
            }
        }

        if (!tid_counts.empty() && tid_counts.size() > 1) {
            std::cout << "\n  Events per TID:" << std::endl;
            for (const auto& [tid, count] : tid_counts) {
                std::cout << "    TID " << tid << ": " << count << " events"
                          << std::endl;
            }
        }

        if (!function_counts.empty()) {
            std::cout << "\n  Top functions by count:" << std::endl;
            std::vector<std::pair<std::string, std::size_t>> sorted_funcs(
                function_counts.begin(), function_counts.end());
            std::sort(sorted_funcs.begin(), sorted_funcs.end(),
                      [](const auto& a, const auto& b) {
                          return a.second > b.second;
                      });

            std::size_t max_display =
                std::min(sorted_funcs.size(), std::size_t(10));
            for (std::size_t i = 0; i < max_display; i++) {
                std::cout << "    " << std::setw(30) << std::left
                          << sorted_funcs[i].first << ": "
                          << sorted_funcs[i].second << std::endl;
            }
        }

        if (!category_counts.empty()) {
            std::cout << "\n  Events per category:" << std::endl;
            for (const auto& [cat, count] : category_counts) {
                std::cout << "    " << std::setw(20) << std::left << cat << ": "
                          << count << std::endl;
            }
        }
    }

    if (!error_messages.empty()) {
        std::cout << "\n=== Errors (" << error_messages.size()
                  << " total) ===" << std::endl;
        std::size_t max_errors =
            std::min(error_messages.size(), std::size_t(10));
        for (std::size_t i = 0; i < max_errors; i++) {
            std::cout << "  " << error_messages[i] << std::endl;
        }
        if (error_messages.size() > 10) {
            std::cout << "  ... and " << (error_messages.size() - 10)
                      << " more errors" << std::endl;
        }
    }

    std::cout << "=====================" << std::endl;
}

}  // namespace dftracer::utils::utilities::replay
