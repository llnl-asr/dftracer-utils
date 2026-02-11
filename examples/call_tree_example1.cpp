/**
 * Example 1: Basic usage of CallTree API with nodes-1 trace
 * Demonstrates all basic operations
 */

#include <dftracer/utils/call_tree/call_tree.h>
#include <cstdio>

using namespace dftracer::utils::call_tree;

int main(int argc, char* argv[]) {
    printf("=== CallTree API Example 1: Basic Usage ===\n");
    printf("\n");
    
    // Check command line arguments
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <trace_directory>\n", argv[0]);
        fprintf(stderr, "Example: %s /path/to/trace/directory\n", argv[0]);
        return 1;
    }
    
    std::string trace_path = argv[1];
    
    printf("Step 1: Load trace files from directory\n");
    CallTree tree;
    if (!tree.load_from_directory(trace_path)) {
        fprintf(stderr, "Failed to load traces from: %s\n", trace_path.c_str());
        return 1;
    }
    printf("  Loaded %zu trace files\n", tree.get_num_trace_files());
    printf("\n");
    
    // Generate call tree
    printf("Step 2: Generate call tree structure\n");
    if (!tree.generate()) {
        fprintf(stderr, "Failed to generate call tree\n");
        return 1;
    }
    printf("\n");
    
    // Print statistics
    printf("Step 3: Print aggregate statistics\n");
    tree.print_statistics();
    
    // Traverse and print in depth-first order
    printf("Step 4: Traverse and print call tree (depth-first, max depth=3)\n");
    tree.print_depth_first(3);  // Limit to 3 levels for readability
    printf("\n");
    
    // Get list of nodes
    printf("Step 5: Get list of nodes in traversal order\n");
    auto nodes = tree.get_nodes_depth_first();
    printf("  Retrieved %zu nodes\n", nodes.size());
    printf("  First 5 nodes:\n");
    for (size_t i = 0; i < std::min(nodes.size(), size_t(5)); i++) {
        const auto& node = nodes[i];
        printf("    [%zu] %s (level=%d, duration=%.3fms)\n",
               i, node.name.c_str(), node.level, static_cast<double>(node.duration_us) / 1000.0);
    }
    printf("\n");
    
    // Save to file
    printf("Step 6: Serialize call tree to binary file\n");
    std::string output_file = tree.get_output_path();
    printf("  Default output path: %s\n", output_file.c_str());
    
    if (tree.save_to_file()) {
        printf("  Successfully saved!\n");
    }
    printf("\n");
    
    // Save to JSON format
    printf("Step 7: Serialize call tree to JSON (Chrome Tracing format)\n");
    if (tree.save_to_json()) {
        printf("  Successfully saved to JSON!\n");
    }
    printf("\n");
    
    // Print tree to text file
    printf("Step 8: Export call tree to text file\n");
    std::string text_file = "nodes-1_calltree.txt";
    if (tree.print_depth_first_to_file(text_file)) {
        printf("  Exported to: %s\n", text_file.c_str());
    }
    
    printf("\n=== Example completed successfully ===\n");
    
    return 0;
}
