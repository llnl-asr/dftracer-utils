#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/mpi/mpi_utils.h>
#include <dftracer/utils/utilities/replay/replay.h>

#include <argparse/argparse.hpp>

#ifdef DFTRACER_UTILS_MPI_ENABLED
#include <mpi.h>
#endif

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::replay;

/**
 * Collect trace files from directory or file list
 */
static std::vector<std::string> collect_trace_files(
    const std::vector<std::string>& inputs, bool recursive) {
    std::vector<std::string> trace_files;

    for (const auto& input : inputs) {
        if (fs::is_directory(input)) {
            if (recursive) {
                for (const auto& entry :
                     fs::recursive_directory_iterator(input)) {
                    if (entry.is_regular_file()) {
                        std::string path = entry.path().string();
                        if ((path.size() >= 4 &&
                             path.substr(path.size() - 4) == ".pfw") ||
                            (path.size() >= 7 &&
                             path.substr(path.size() - 7) == ".pfw.gz")) {
                            trace_files.push_back(path);
                        }
                    }
                }
            } else {
                for (const auto& entry : fs::directory_iterator(input)) {
                    if (entry.is_regular_file()) {
                        std::string path = entry.path().string();
                        if ((path.size() >= 4 &&
                             path.substr(path.size() - 4) == ".pfw") ||
                            (path.size() >= 7 &&
                             path.substr(path.size() - 7) == ".pfw.gz")) {
                            trace_files.push_back(path);
                        }
                    }
                }
            }
        } else if (fs::is_regular_file(input)) {
            trace_files.push_back(input);
        } else {
            DFTRACER_UTILS_LOG_ERROR("Input not found or not accessible: %s",
                                     input.c_str());
        }
    }

    return trace_files;
}

int main(int argc, char** argv) {
#ifdef DFTRACER_UTILS_MPI_ENABLED
    MPI_Init(&argc, &argv);
    mpi::MPIUtils::instance().initialize();
#endif

    DFTRACER_UTILS_LOGGER_INIT();

    // Get MPI rank for output control (defaults to rank 0, size 1 without MPI)
    int mpi_rank = 0;
    int mpi_size = 1;
    bool is_root = true;
#ifdef DFTRACER_UTILS_MPI_ENABLED
    mpi_rank = mpi::MPIUtils::instance().get_rank();
    mpi_size = mpi::MPIUtils::instance().get_world_size();
    is_root = mpi::MPIUtils::instance().is_root();
#endif

    argparse::ArgumentParser program("dftracer_replay",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "DFTracer replay utility - replays I/O operations from DFTracer trace "
        "files (.pfw, .pfw.gz)");

    // Input files/directories
    program.add_argument("inputs")
        .help(
            "Trace files (.pfw, .pfw.gz) or directories containing trace files")
        .nargs(argparse::nargs_pattern::at_least_one);

    // Timing options
    program.add_argument("--no-timing")
        .help("Ignore original timing and execute as fast as possible")
        .flag();

    // Execution options
    program.add_argument("--dry-run")
        .help("Parse and analyze traces without executing operations")
        .flag();

    program.add_argument("--dftracer-mode")
        .help(
            "Use DFTracer sleep-based replay (sleep for operation duration "
            "instead of doing actual I/O)")
        .flag();

    program.add_argument("--no-sleep")
        .help(
            "When used with --dftracer-mode, disable sleep calls for maximum "
            "speed")
        .flag();

    program.add_argument("--verbose")
        .help("Enable verbose output and detailed statistics")
        .flag();

    program.add_argument("-r", "--recursive")
        .help("Recursively search directories for trace files")
        .flag();

    // Call tree options
    program.add_argument("--use-call-tree")
        .help("Build and use call tree structure for hierarchical replay")
        .flag();

    program.add_argument("--hierarchical-replay")
        .help(
            "Replay operations respecting parent-child call hierarchy "
            "(requires --use-call-tree)")
        .flag();

    program.add_argument("--respect-call-hierarchy")
        .help(
            "Replay child nodes immediately after parent (requires "
            "--use-call-tree and --hierarchical-replay)")
        .flag();

    // Filtering options - Process/Thread
    program.add_argument("--filter-pid")
        .help("Only replay events from specific PID(s) (comma-separated)")
        .default_value(std::string(""));

    program.add_argument("--exclude-pid")
        .help("Exclude events from specific PID(s) (comma-separated)")
        .default_value(std::string(""));

    program.add_argument("--filter-tid")
        .help("Only replay events from specific TID(s) (comma-separated)")
        .default_value(std::string(""));

    program.add_argument("--exclude-tid")
        .help("Exclude events from specific TID(s) (comma-separated)")
        .default_value(std::string(""));

    // Filtering options - Function/Category
    program.add_argument("--filter-function")
        .help(
            "Only replay specific function(s) (comma-separated, e.g., "
            "'read,write,open')")
        .default_value(std::string(""));

    program.add_argument("--exclude-function")
        .help("Exclude specific function(s) (comma-separated)")
        .default_value(std::string(""));

    program.add_argument("--filter-category")
        .help(
            "Only replay specific category/categories (comma-separated, e.g., "
            "'POSIX,storage')")
        .default_value(std::string(""));

    program.add_argument("--exclude-category")
        .help("Exclude specific category/categories (comma-separated)")
        .default_value(std::string(""));

    // Filtering options - Timestamp
    program.add_argument("--start-timestamp")
        .help("Only replay events after this timestamp (microseconds)")
        .default_value(std::uint64_t(0))
        .scan<'u', std::uint64_t>();

    program.add_argument("--end-timestamp")
        .help("Only replay events before this timestamp (microseconds)")
        .default_value(UINT64_MAX)
        .scan<'u', std::uint64_t>();

    // Filtering options - Size
    program.add_argument("--min-size")
        .help("Only replay operations with size >= this value (bytes)")
        .default_value(std::int64_t(-1))
        .scan<'i', std::int64_t>();

    program.add_argument("--max-size")
        .help("Only replay operations with size <= this value (bytes)")
        .default_value(std::int64_t(-1))
        .scan<'i', std::int64_t>();

    // Sampling options
    program.add_argument("--sample-rate")
        .help("Sample rate for replay (0.0-1.0, 1.0=all events, 0.1=10%)")
        .default_value(1.0)
        .scan<'g', double>();

    program.add_argument("--sample-seed")
        .help("Random seed for sampling (for reproducibility)")
        .default_value(std::uint64_t(0))
        .scan<'u', std::uint64_t>();

    // Resource limits
    program.add_argument("--max-events")
        .help("Maximum number of events to replay (0=unlimited)")
        .default_value(std::size_t(0))
        .scan<'u', std::size_t>();

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Argument parsing error: %s", err.what());
        std::cerr << program;
        return 1;
    }

    // Helper to parse comma-separated values
    auto parse_csv_uint32 =
        [](const std::string& csv) -> std::unordered_set<std::uint32_t> {
        std::unordered_set<std::uint32_t> result;
        if (csv.empty()) return result;

        std::istringstream ss(csv);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (!token.empty()) {
                result.insert(static_cast<std::uint32_t>(std::stoul(token)));
            }
        }
        return result;
    };

    auto parse_csv_string =
        [](const std::string& csv) -> std::unordered_set<std::string> {
        std::unordered_set<std::string> result;
        if (csv.empty()) return result;

        std::istringstream ss(csv);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (!token.empty()) {
                result.insert(token);
            }
        }
        return result;
    };

    // Parse arguments
    std::vector<std::string> inputs =
        program.get<std::vector<std::string>>("inputs");
    bool no_timing = program.get<bool>("--no-timing");
    bool dry_run = program.get<bool>("--dry-run");
    bool dftracer_mode = program.get<bool>("--dftracer-mode");
    bool no_sleep = program.get<bool>("--no-sleep");
    bool verbose = program.get<bool>("--verbose");
    bool recursive = program.get<bool>("--recursive");

    // Call tree options
    bool use_call_tree = program.get<bool>("--use-call-tree");
    bool hierarchical_replay = program.get<bool>("--hierarchical-replay");
    bool respect_call_hierarchy = program.get<bool>("--respect-call-hierarchy");

    // Parse filter arguments
    auto filter_pids =
        parse_csv_uint32(program.get<std::string>("--filter-pid"));
    auto exclude_pids =
        parse_csv_uint32(program.get<std::string>("--exclude-pid"));
    auto filter_tids =
        parse_csv_uint32(program.get<std::string>("--filter-tid"));
    auto exclude_tids =
        parse_csv_uint32(program.get<std::string>("--exclude-tid"));
    auto filter_functions =
        parse_csv_string(program.get<std::string>("--filter-function"));
    auto exclude_functions =
        parse_csv_string(program.get<std::string>("--exclude-function"));
    auto filter_categories =
        parse_csv_string(program.get<std::string>("--filter-category"));
    auto exclude_categories =
        parse_csv_string(program.get<std::string>("--exclude-category"));

    std::uint64_t start_timestamp =
        program.get<std::uint64_t>("--start-timestamp");
    std::uint64_t end_timestamp = program.get<std::uint64_t>("--end-timestamp");
    std::int64_t min_size = program.get<std::int64_t>("--min-size");
    std::int64_t max_size = program.get<std::int64_t>("--max-size");
    double sample_rate = program.get<double>("--sample-rate");
    std::uint64_t sample_seed = program.get<std::uint64_t>("--sample-seed");
    std::size_t max_events = program.get<std::size_t>("--max-events");

    // Validate --no-sleep usage
    if (no_sleep && !dftracer_mode) {
        std::cerr << "Error: --no-sleep can only be used with --dftracer-mode"
                  << std::endl;
        return 1;
    }

    // Validate call tree options
    if (hierarchical_replay && !use_call_tree) {
        std::cerr << "Error: --hierarchical-replay requires --use-call-tree"
                  << std::endl;
        return 1;
    }

    if (respect_call_hierarchy && !hierarchical_replay) {
        std::cerr
            << "Error: --respect-call-hierarchy requires --hierarchical-replay"
            << std::endl;
        return 1;
    }

    // Validate sample rate
    if (sample_rate < 0.0 || sample_rate > 1.0) {
        std::cerr << "Error: --sample-rate must be between 0.0 and 1.0"
                  << std::endl;
        return 1;
    }

    // Collect trace files
    std::vector<std::string> trace_files =
        collect_trace_files(inputs, recursive);

    if (trace_files.empty()) {
        if (is_root)
            std::cerr << "No trace files found in the specified inputs."
                      << std::endl;
#ifdef DFTRACER_UTILS_MPI_ENABLED
        mpi::MPIUtils::instance().finalize();
        MPI_Finalize();
#endif
        return 1;
    }

    if (is_root) {
        std::cout << "Found " << trace_files.size()
                  << " trace file(s) to replay:" << std::endl;
        for (const auto& file : trace_files) {
            std::cout << "  " << file << std::endl;
        }
    }

    // Configure replay
    ReplayConfig config;
    config.maintain_timing = !no_timing;
    config.dry_run = dry_run;
    config.dftracer_mode = dftracer_mode;
    config.no_sleep = no_sleep;
    config.verbose = verbose;

    // Store MPI info in config
    config.mpi_rank = mpi_rank;
    config.mpi_size = mpi_size;

    // Call tree options
    config.use_call_tree = use_call_tree;
    config.hierarchical_replay = hierarchical_replay;
    config.respect_call_hierarchy = respect_call_hierarchy;

    // Apply filters
    config.filter_pids = filter_pids;
    config.exclude_pids = exclude_pids;
    config.filter_tids = filter_tids;
    config.exclude_tids = exclude_tids;
    config.filter_functions = filter_functions;
    config.exclude_functions = exclude_functions;
    config.filter_categories = filter_categories;
    config.exclude_categories = exclude_categories;

    // Apply ranges and limits
    config.start_timestamp = start_timestamp;
    config.end_timestamp = end_timestamp;
    config.min_operation_size = min_size;
    config.max_operation_size = max_size;
    config.sampling_rate = sample_rate;
    config.sample_seed = sample_seed;
    config.max_events = max_events;

    // Print configuration (only on rank 0)
    if (is_root) {
        std::cout << "\n=== Replay Configuration ===" << std::endl;
        if (mpi_size > 1) {
            std::cout << "MPI processes: " << mpi_size << std::endl;
        }
        std::cout << "Maintain timing: "
                  << (config.maintain_timing ? "yes" : "no") << std::endl;
        std::cout << "Dry run: " << (config.dry_run ? "yes" : "no")
                  << std::endl;
        if (config.dftracer_mode) {
            std::cout << "DFTracer mode: yes ("
                      << (config.no_sleep ? "no-sleep" : "sleep-based") << ")"
                      << std::endl;
        } else {
            std::cout << "DFTracer mode: no (actual I/O)" << std::endl;
        }
        if (config.use_call_tree) {
            std::cout << "Call tree mode: yes" << std::endl;
            std::cout << "  Hierarchical replay: "
                      << (config.hierarchical_replay ? "yes" : "no")
                      << std::endl;
            if (config.hierarchical_replay) {
                std::cout << "  Respect call hierarchy: "
                          << (config.respect_call_hierarchy ? "yes" : "no")
                          << std::endl;
            }
        }
    }
    // Print active filters
    // Print active filters (only on rank 0)
    if (is_root &&
        (!filter_pids.empty() || !exclude_pids.empty() ||
         !filter_tids.empty() || !exclude_tids.empty() ||
         !filter_functions.empty() || !exclude_functions.empty() ||
         !filter_categories.empty() || !exclude_categories.empty() ||
         start_timestamp > 0 || end_timestamp < UINT64_MAX || min_size >= 0 ||
         max_size >= 0 || sample_rate < 1.0 || max_events > 0)) {
        std::cout << "\nActive Filters:" << std::endl;
        if (!filter_pids.empty()) {
            std::cout << "  Filter PIDs: ";
            for (auto pid : filter_pids) std::cout << pid << " ";
            std::cout << std::endl;
        }
        if (!exclude_pids.empty()) {
            std::cout << "  Exclude PIDs: ";
            for (auto pid : exclude_pids) std::cout << pid << " ";
            std::cout << std::endl;
        }
        if (!filter_tids.empty()) {
            std::cout << "  Filter TIDs: ";
            for (auto tid : filter_tids) std::cout << tid << " ";
            std::cout << std::endl;
        }
        if (!exclude_tids.empty()) {
            std::cout << "  Exclude TIDs: ";
            for (auto tid : exclude_tids) std::cout << tid << " ";
            std::cout << std::endl;
        }
        if (!filter_functions.empty()) {
            std::cout << "  Filter functions: ";
            for (const auto& f : filter_functions) std::cout << f << " ";
            std::cout << std::endl;
        }
        if (!exclude_functions.empty()) {
            std::cout << "  Exclude functions: ";
            for (const auto& f : exclude_functions) std::cout << f << " ";
            std::cout << std::endl;
        }
        if (!filter_categories.empty()) {
            std::cout << "  Filter categories: ";
            for (const auto& c : filter_categories) std::cout << c << " ";
            std::cout << std::endl;
        }
        if (!exclude_categories.empty()) {
            std::cout << "  Exclude categories: ";
            for (const auto& c : exclude_categories) std::cout << c << " ";
            std::cout << std::endl;
        }
        if (start_timestamp > 0) {
            std::cout << "  Start timestamp: " << start_timestamp << std::endl;
        }
        if (end_timestamp < UINT64_MAX) {
            std::cout << "  End timestamp: " << end_timestamp << std::endl;
        }
        if (min_size >= 0) {
            std::cout << "  Min operation size: " << min_size << " bytes"
                      << std::endl;
        }
        if (max_size >= 0) {
            std::cout << "  Max operation size: " << max_size << " bytes"
                      << std::endl;
        }
        if (sample_rate < 1.0) {
            std::cout << "  Sampling rate: " << (sample_rate * 100.0) << "%"
                      << std::endl;
        }
        if (max_events > 0) {
            std::cout << "  Max events: " << max_events << std::endl;
        }
    }

    // Create replay engine and execute
    if (is_root) std::cout << "\n=== Starting Replay ===" << std::endl;

    auto start_time = std::chrono::steady_clock::now();

    ReplayEngine engine(config);
    ReplayResult result;

    if (use_call_tree) {
        // Call tree mode: expects a single directory containing trace files
        if (inputs.size() != 1 || !fs::is_directory(inputs[0])) {
            if (is_root)
                std::cerr << "Error: --use-call-tree requires exactly one "
                             "input directory"
                          << std::endl;
#ifdef DFTRACER_UTILS_MPI_ENABLED
            mpi::MPIUtils::instance().finalize();
            MPI_Finalize();
#endif
            return 1;
        }
        if (is_root)
            std::cout << "Using call tree hierarchical replay mode"
                      << std::endl;
        result = engine.replay_with_call_tree(inputs[0]);
    } else {
        // Normal mode: replay trace files directly
        result = engine.replay(trace_files);
    }

    auto end_time = std::chrono::steady_clock::now();
    auto total_wall_time =
        std::chrono::duration_cast<std::chrono::microseconds>(end_time -
                                                              start_time);

    if (is_root) {
        std::cout << "\n=== Replay Completed ===" << std::endl;
        std::cout << "Wall clock time: "
                  << static_cast<double>(total_wall_time.count()) / 1000.0
                  << " ms" << std::endl;

        // Print results
        result.print_summary(verbose);
    }

    // Return appropriate exit code
    int exit_code = 0;
    if (result.failed_events > 0) {
        if (is_root)
            std::cout << "\nReplay completed with errors." << std::endl;
        exit_code = 2;
    } else if (result.executed_events > 0) {
        if (is_root)
            std::cout << "\nReplay completed successfully." << std::endl;
        exit_code = 0;
    } else {
        if (is_root) std::cout << "\nNo events were executed." << std::endl;
        exit_code = 1;
    }

#ifdef DFTRACER_UTILS_MPI_ENABLED
    mpi::MPIUtils::instance().finalize();
    MPI_Finalize();
#endif

    return exit_code;
}
