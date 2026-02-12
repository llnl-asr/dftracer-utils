/**
 * Example 2: Advanced usage with nodes-4 trace
 * Demonstrates handling larger traces with multiple nodes
 */

#include <dftracer/utils/call_tree/call_tree.h>
#include <cstdio>
#include <map>

using namespace dftracer::utils::call_tree;

int main(int argc, char* argv[]) {
    printf("=== CallTree API Example 2: Multi-Node Traces ===\n");
    printf("\n");
    
    // Check command line arguments
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <trace_directory>\n", argv[0]);
        fprintf(stderr, "Example: %s /path/to/trace/directory\n", argv[0]);
        return 1;
    }
    
    std::string trace_path = argv[1];
    
    printf("Loading traces from: %s\n", trace_path.c_str());
    printf("\n");
    
    CallTree tree;
    
    // Load with custom pattern
    if (!tree.load_from_directory(trace_path, "*.pfw.gz")) {
        fprintf(stderr, "Failed to load traces\n");
        return 1;
    }
    
    printf("Found %zu trace files\n", tree.get_num_trace_files());
    printf("\n");
    
    // Generate call tree
    printf("Generating call tree...\n");
    if (!tree.generate()) {
        fprintf(stderr, "Failed to generate call tree\n");
        return 1;
    }
    
    // Get detailed statistics
    printf("\n--- Detailed Statistics ---\n");
    auto stats = tree.get_statistics();
    
    printf("Total nodes across all processes: %zu\n", stats.total_nodes);
    printf("Number of tree levels: %zu\n", stats.num_levels);
    printf("Leaf nodes: %zu\n", stats.num_leaf_nodes);
    printf("Unique process/thread combinations: %zu\n", stats.num_processes);
    printf("\n");
    
    // Analyze per-level information
    printf("--- Per-Level Analysis ---\n");
    printf("%-10s%-15s%-20s%-15s\n", "Level", "Node Count", "Avg Time (ms)", "% of Total");
    printf("------------------------------------------------------------\n");
    
    for (size_t i = 0; i < stats.num_levels; i++) {
        double percent = (stats.total_nodes == 0)
            ? 0.0
            : (static_cast<double>(stats.nodes_per_level[i]) / static_cast<double>(stats.total_nodes)) * 100.0;
        printf("%-10zu%-15zu%-20.3f%-14.1f%%\n",
               i, stats.nodes_per_level[i],
               stats.avg_time_per_level_us[i] / 1000.0, percent);
    }
    printf("\n");
    
    // Get all nodes and analyze
    printf("--- Node Analysis ---\n");
    auto nodes = tree.get_nodes_depth_first();
    
    // Count by category
    std::map<std::string, size_t> category_counts;
    for (const auto& node : nodes) {
        category_counts[node.category]++;
    }
    
    printf("Nodes by category:\n");
    for (const auto& [category, count] : category_counts) {
        printf("  %-20s: %zu nodes\n", category.c_str(), count);
    }
    printf("\n");
    
    // Save outputs
    printf("--- Saving Outputs ---\n");
    
    // Set custom output path
    tree.set_output_path("nodes-4_calltree.bin");
    
    if (tree.save_to_file()) {
        printf("Binary format saved: nodes-4_calltree.bin\n");
    }
    
    // Save to JSON (Chrome Tracing format)
    if (tree.save_to_json("nodes-4_calltree.pfw")) {
        printf("JSON format saved: nodes-4_calltree.pfw (Chrome Tracing compatible)\n");
    }
    
    if (tree.print_depth_first_to_file("nodes-4_calltree_full.txt", 0)) {
        printf("Full tree saved: nodes-4_calltree_full.txt\n");
    }
    
    if (tree.print_depth_first_to_file("nodes-4_calltree_summary.txt", 2)) {
        printf("Summary (2 levels) saved: nodes-4_calltree_summary.txt\n");
    }
    
    printf("\n=== Example completed successfully ===\n");
    
    return 0;
}
