/**
 * DFTracer Call Tree Utility
 * Standalone binary for building and analyzing call trees from DFTracer trace
 * files
 */

#include <dftracer/utils/call_tree/call_tree.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>

#include <algorithm>
#include <argparse/argparse.hpp>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace dftracer::utils::call_tree;

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

/**
 * Analyze call patterns in the tree
 */
static void analyze_call_patterns(const std::vector<CallTreeNodeInfo>& nodes) {
    printf("\n--- Call Pattern Analysis ---\n");

    if (nodes.empty()) {
        printf("No nodes to analyze\n");
        return;
    }

    // Find most frequently called functions
    std::map<std::string, size_t> call_counts;
    for (const auto& node : nodes) {
        call_counts[node.name]++;
    }

    // Sort by frequency
    std::vector<std::pair<std::string, size_t>> sorted_calls(
        call_counts.begin(), call_counts.end());
    std::sort(sorted_calls.begin(), sorted_calls.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    printf("Top 10 most frequently called functions:\n");
    for (size_t i = 0; i < std::min(sorted_calls.size(), size_t(10)); i++) {
        printf("  %2zu. %-30s : %zu calls\n", i + 1,
               sorted_calls[i].first.c_str(), sorted_calls[i].second);
    }
}

/**
 * Analyze timing statistics
 */
static void analyze_timing(const std::vector<CallTreeNodeInfo>& nodes) {
    printf("\n--- Timing Analysis ---\n");

    if (nodes.empty()) {
        printf("No nodes to analyze\n");
        return;
    }

    // Calculate timing statistics
    std::vector<std::uint64_t> durations;
    durations.reserve(nodes.size());

    for (const auto& node : nodes) {
        durations.push_back(node.duration_us);
    }

    std::sort(durations.begin(), durations.end());

    std::uint64_t total = 0;
    for (auto d : durations) {
        total += d;
    }
    double avg =
        static_cast<double>(total) / static_cast<double>(durations.size());

    std::uint64_t min_time = durations.front();
    std::uint64_t max_time = durations.back();
    std::uint64_t median = durations[durations.size() / 2];
    std::uint64_t p95 = durations[static_cast<size_t>(
        static_cast<double>(durations.size()) * 0.95)];
    std::uint64_t p99 = durations[static_cast<size_t>(
        static_cast<double>(durations.size()) * 0.99)];

    printf("Duration statistics (milliseconds):\n");
    printf("  Min:    %.3f ms\n", static_cast<double>(min_time) / 1000.0);
    printf("  Max:    %.3f ms\n", static_cast<double>(max_time) / 1000.0);
    printf("  Mean:   %.3f ms\n", avg / 1000.0);
    printf("  Median: %.3f ms\n", static_cast<double>(median) / 1000.0);
    printf("  95th:   %.3f ms\n", static_cast<double>(p95) / 1000.0);
    printf("  99th:   %.3f ms\n", static_cast<double>(p99) / 1000.0);
}

/**
 * Find critical path (longest duration calls)
 */
static void find_critical_path(const std::vector<CallTreeNodeInfo>& nodes) {
    printf("\n--- Critical Path (Longest Duration Calls) ---\n");

    if (nodes.empty()) {
        printf("No nodes to analyze\n");
        return;
    }

    // Find top 10 longest running calls
    std::vector<CallTreeNodeInfo> sorted_nodes = nodes;
    std::sort(sorted_nodes.begin(), sorted_nodes.end(),
              [](const auto& a, const auto& b) {
                  return a.duration_us > b.duration_us;
              });

    printf("Top 10 longest running calls:\n");
    for (size_t i = 0; i < std::min(sorted_nodes.size(), size_t(10)); i++) {
        const auto& node = sorted_nodes[i];
        printf("  %2zu. %-30s [%-15s] - %10.3f ms (level %d)\n", i + 1,
               node.name.c_str(), node.category.c_str(),
               static_cast<double>(node.duration_us) / 1000.0, node.level);
    }
}

/**
 * Analyze by category
 */
static void analyze_by_category(const std::vector<CallTreeNodeInfo>& nodes) {
    printf("\n--- Analysis by Category ---\n");

    if (nodes.empty()) {
        printf("No nodes to analyze\n");
        return;
    }

    std::map<std::string, size_t> category_counts;
    std::map<std::string, std::uint64_t> category_durations;

    for (const auto& node : nodes) {
        category_counts[node.category]++;
        category_durations[node.category] += node.duration_us;
    }

    printf("Nodes by category:\n");
    for (const auto& [category, count] : category_counts) {
        double avg_duration =
            static_cast<double>(category_durations[category]) /
            static_cast<double>(count) / 1000.0;
        printf("  %-20s: %6zu nodes, avg duration: %.3f ms\n", category.c_str(),
               count, avg_duration);
    }
}

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    argparse::ArgumentParser program("dftracer_call_tree",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "DFTracer Call Tree utility - builds and analyzes call trees from "
        "DFTracer trace files");

    // Input files/directories
    program.add_argument("inputs")
        .help(
            "Trace files (.pfw, .pfw.gz) or directories containing trace files")
        .nargs(argparse::nargs_pattern::at_least_one);

    // Processing options
    program.add_argument("-r", "--recursive")
        .help("Recursively search directories for trace files")
        .flag();

    program.add_argument("--pattern")
        .help("File pattern for trace files (default: *.pfw.gz)")
        .default_value(std::string("*.pfw.gz"));

    // Output options
    program.add_argument("-o", "--output")
        .help(
            "Output file path for serialized call tree (default: "
            "auto-generated from input)")
        .default_value(std::string(""));

    program.add_argument("--json")
        .help("Also save call tree in JSON (Chrome Tracing) format")
        .flag();

    program.add_argument("--text")
        .help("Export call tree to text file")
        .default_value(std::string(""));

    // Analysis options
    program.add_argument("--max-depth")
        .help("Maximum depth for tree printing (0=unlimited)")
        .default_value(0)
        .scan<'i', int>();

    program.add_argument("--analyze")
        .help(
            "Perform detailed analysis (call patterns, timing, critical path)")
        .flag();

    program.add_argument("-v", "--verbose")
        .help("Enable verbose output")
        .flag();

    program.add_argument("--stats-only")
        .help("Only print statistics, skip tree traversal")
        .flag();

    program.add_argument("--no-save")
        .help("Don't save output files, only print analysis")
        .flag();

    // Parse arguments
    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        return 1;
    }

    // Get arguments
    auto inputs = program.get<std::vector<std::string>>("inputs");
    bool recursive = program.get<bool>("--recursive");
    std::string pattern = program.get<std::string>("--pattern");
    std::string output_path = program.get<std::string>("--output");
    bool save_json = program.get<bool>("--json");
    std::string text_file = program.get<std::string>("--text");
    int max_depth = program.get<int>("--max-depth");
    bool analyze = program.get<bool>("--analyze");
    bool verbose = program.get<bool>("--verbose");
    bool stats_only = program.get<bool>("--stats-only");
    bool no_save = program.get<bool>("--no-save");

    // Collect trace files
    printf("=== DFTracer Call Tree Builder ===\n\n");

    auto start_time = std::chrono::high_resolution_clock::now();

    // For single directory input, use load_from_directory
    // For multiple inputs or files, collect manually
    CallTree tree;
    bool loaded = false;

    if (inputs.size() == 1 && fs::is_directory(inputs[0])) {
        printf("Loading traces from directory: %s\n", inputs[0].c_str());
        if (verbose) {
            printf("  Pattern: %s\n", pattern.c_str());
            printf("  Recursive: %s\n", recursive ? "yes" : "no");
        }

        loaded = tree.load_from_directory(inputs[0], pattern);
        if (!loaded) {
            fprintf(stderr, "Failed to load traces from directory: %s\n",
                    inputs[0].c_str());
            return 1;
        }
    } else {
        auto trace_files = collect_trace_files(inputs, recursive);
        if (trace_files.empty()) {
            fprintf(stderr, "No trace files found in the specified inputs.\n");
            return 1;
        }

        printf("Found %zu trace file(s) to process:\n", trace_files.size());
        if (verbose) {
            for (const auto& file : trace_files) {
                printf("  %s\n", file.c_str());
            }
        }

        // Load first directory for now (CallTree API expects directory)
        // This is a limitation of the current API
        fprintf(stderr,
                "Note: Multi-file input not yet supported. Use directory input "
                "instead.\n");
        return 1;
    }

    printf("Loaded %zu trace files\n", tree.get_num_trace_files());
    printf("\n");

    // Generate call tree
    printf("Generating call tree structure...\n");
    if (!tree.generate()) {
        fprintf(stderr, "Failed to generate call tree\n");
        return 1;
    }
    printf("Call tree generation complete\n\n");

    auto gen_time = std::chrono::high_resolution_clock::now();
    auto gen_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        gen_time - start_time);
    if (verbose) {
        printf("Generation time: %lld ms\n\n",
               static_cast<long long>(gen_duration.count()));
    }

    // Print statistics
    printf("=== Call Tree Statistics ===\n");
    tree.print_statistics();

    // Print tree structure
    if (!stats_only) {
        printf("\n=== Call Tree Structure ===\n");
        tree.print_depth_first(max_depth);
    }

    // Perform detailed analysis if requested
    if (analyze) {
        printf("\n=== Detailed Analysis ===\n");
        auto nodes = tree.get_nodes_depth_first();
        printf("Retrieved %zu nodes for analysis\n", nodes.size());

        analyze_call_patterns(nodes);
        analyze_timing(nodes);
        find_critical_path(nodes);
        analyze_by_category(nodes);
    }

    // Save outputs
    if (!no_save) {
        printf("\n=== Saving Outputs ===\n");

        // Set custom output path if specified
        if (!output_path.empty()) {
            tree.set_output_path(output_path);
        }

        // Save binary format
        std::string bin_file = tree.get_output_path();
        printf("Saving binary call tree to: %s\n", bin_file.c_str());
        if (tree.save_to_file()) {
            printf("  Successfully saved!\n");
        } else {
            fprintf(stderr, "  Failed to save binary file\n");
        }

        // Save JSON format if requested
        if (save_json) {
            std::string json_file = bin_file;
            // Replace .calltree extension with .pfw
            if (json_file.size() >= 9 &&
                json_file.substr(json_file.size() - 9) == ".calltree") {
                json_file = json_file.substr(0, json_file.size() - 9) + ".pfw";
            } else {
                json_file += ".pfw";
            }

            printf("Saving JSON call tree to: %s\n", json_file.c_str());
            if (tree.save_to_json(json_file)) {
                printf("  Successfully saved! (Chrome Tracing compatible)\n");
            } else {
                fprintf(stderr, "  Failed to save JSON file\n");
            }
        }

        // Save text format if requested
        if (!text_file.empty()) {
            printf("Exporting call tree to text file: %s\n", text_file.c_str());
            if (tree.print_depth_first_to_file(text_file, max_depth)) {
                printf("  Successfully exported!\n");
            } else {
                fprintf(stderr, "  Failed to export text file\n");
            }
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);

    printf("\n=== Completed ===\n");
    printf("Total execution time: %lld ms\n",
           static_cast<long long>(total_duration.count()));

    return 0;
}
