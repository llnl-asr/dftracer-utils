/**
 * Example 3: Using CallTree API programmatically
 * Shows how to use node information for custom analysis
 */

#include <dftracer/utils/call_tree/call_tree.h>
#include <cstdio>
#include <algorithm>
#include <numeric>
#include <map>

using namespace dftracer::utils::call_tree;

static void analyze_call_patterns(const std::vector<CallTreeNodeInfo>& nodes) {
    printf("\n--- Call Pattern Analysis ---\n");
    
    // Find most frequently called functions
    std::map<std::string, size_t> call_counts;
    for (const auto& node : nodes) {
        call_counts[node.name]++;
    }
    
    // Sort by frequency
    std::vector<std::pair<std::string, size_t>> sorted_calls(call_counts.begin(), call_counts.end());
    std::sort(sorted_calls.begin(), sorted_calls.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    
    printf("Top 10 most frequently called functions:\n");
    for (size_t i = 0; i < std::min(sorted_calls.size(), size_t(10)); i++) {
        printf("  %zu. %s (%zu calls)\n",
               i+1, sorted_calls[i].first.c_str(), sorted_calls[i].second);
    }
}

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
    
    std::uint64_t total = std::accumulate(durations.begin(), durations.end(), 0ULL);
    double avg = static_cast<double>(total) / static_cast<double>(durations.size());
    
    std::uint64_t min_time = durations.front();
    std::uint64_t max_time = durations.back();
    std::uint64_t median = durations[durations.size() / 2];
    std::uint64_t p95 = durations[static_cast<size_t>(static_cast<double>(durations.size()) * 0.95)];
    std::uint64_t p99 = durations[static_cast<size_t>(static_cast<double>(durations.size()) * 0.99)];
    
    printf("Duration statistics (milliseconds):\n");
    printf("  Min:    %.3f ms\n", static_cast<double>(min_time) / 1000.0);
    printf("  Max:    %.3f ms\n", static_cast<double>(max_time) / 1000.0);
    printf("  Mean:   %.3f ms\n", avg / 1000.0);
    printf("  Median: %.3f ms\n", static_cast<double>(median) / 1000.0);
    printf("  95th:   %.3f ms\n", static_cast<double>(p95) / 1000.0);
    printf("  99th:   %.3f ms\n", static_cast<double>(p99) / 1000.0);
}

static void find_critical_path(const std::vector<CallTreeNodeInfo>& nodes) {
    printf("\n--- Critical Path (Longest Duration Chain) ---\n");
    
    // Find top 10 longest running calls
    std::vector<CallTreeNodeInfo> sorted_nodes = nodes;
    std::sort(sorted_nodes.begin(), sorted_nodes.end(),
              [](const auto& a, const auto& b) { return a.duration_us > b.duration_us; });
    
    printf("Top 10 longest running calls:\n");
    for (size_t i = 0; i < std::min(sorted_nodes.size(), size_t(10)); i++) {
        const auto& node = sorted_nodes[i];
        printf("  %zu. %s [%s] - %.3f ms (level %d)\n",
               i+1, node.name.c_str(), node.category.c_str(),
               static_cast<double>(node.duration_us) / 1000.0, node.level);
    }
}

int main(int argc, char* argv[]) {
    printf("=== CallTree API Example 3: Custom Analysis ===\n");
    
    // Check command line arguments
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <trace_directory>\n", argv[0]);
        fprintf(stderr, "Example: %s /path/to/trace/directory\n", argv[0]);
        return 1;
    }
    
    std::string trace_path = argv[1];
    
    CallTree tree;
    
    printf("\nLoading and generating call tree...\n");
    if (!tree.load_from_directory(trace_path)) {
        fprintf(stderr, "Failed to load traces\n");
        return 1;
    }
    
    if (!tree.generate()) {
        fprintf(stderr, "Failed to generate call tree\n");
        return 1;
    }
    
    // Get all nodes for analysis
    auto nodes = tree.get_nodes_depth_first();
    printf("Analyzing %zu nodes...\n", nodes.size());
    
    // Perform custom analyses
    analyze_call_patterns(nodes);
    analyze_timing(nodes);
    find_critical_path(nodes);
    
    // Also print the built-in statistics
    tree.print_statistics();
    
    // Save analysis results in JSON format for downstream processing
    printf("\nSaving analysis results...\n");
    if (tree.save_to_json("analysis_output.pfw")) {
        printf("✓ JSON output saved to: analysis_output.pfw\n");
        printf("  This file can be imported into Chrome Tracing, Perfetto,\n");
        printf("  or analyzed with DFAnalyzer tools.\n");
    }
    
    printf("\n=== Analysis complete ===\n");
    
    return 0;
}
