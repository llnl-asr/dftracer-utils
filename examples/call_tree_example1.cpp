/**
 * Example 1: Basic usage of CallTree API with nodes-1 trace
 * Demonstrates all basic operations
 */

#include <dftracer/utils/call_tree/call_tree.h>
#include <dftracer/utils/call_tree/internal/call_tree.h>
#include <dftracer/utils/call_tree/mpi/serializable.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>

#include <cstdio>

using namespace dftracer::utils::call_tree;
using dftracer::utils::CoroScope;
using dftracer::utils::Pipeline;
using dftracer::utils::make_task;
namespace coro = dftracer::utils::coro;

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
    
    // Step 6: persist via the coroutine save APIs driven by a Pipeline.
    printf("Step 6: Save call tree (custom binary + Arrow IPC)\n");
    const std::string bin_path = "nodes-1_calltree.bin";
    const std::string arrow_path = "nodes-1_calltree.arrow";
    bool bin_ok = false, arrow_ok = false;
    {
        Pipeline pipeline;
        auto save = make_task(
            [&](CoroScope& scope) -> coro::CoroTask<void> {
                bin_ok = co_await save_binary(&scope, tree.internal_tree(),
                                              bin_path);
                arrow_ok = co_await save_arrow(&scope, tree.internal_tree(),
                                               arrow_path);
            },
            "save_call_tree");
        pipeline.set_source(save);
        pipeline.set_destination(save);
        pipeline.execute();
    }
    printf("  Binary: %s -> %s\n", bin_path.c_str(), bin_ok ? "ok" : "failed");
    printf("  Arrow:  %s -> %s\n", arrow_path.c_str(),
           arrow_ok ? "ok" : "failed");

    printf("\n=== Example completed successfully ===\n");
    
    return 0;
}
