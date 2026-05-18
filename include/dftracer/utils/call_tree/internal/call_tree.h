#ifndef DFTRACER_UTILS_CALL_TREE_INTERNAL_CALL_TREE_H
#define DFTRACER_UTILS_CALL_TREE_INTERNAL_CALL_TREE_H

#include <dftracer/utils/call_tree/internal/factory.h>
#include <dftracer/utils/call_tree/internal/node.h>
#include <dftracer/utils/call_tree/internal/process_call_tree.h>
#include <dftracer/utils/call_tree/internal/process_key.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::call_tree {
namespace internal {

// Forward declaration
class TraceReader;

/**
 * Main call graph - Container for all process call graphs
 * Acts as a map-like structure that returns ProcessCallTree nodes by ProcessKey
 * Modern C++ API design:
 * - Constructor takes log file (no separate load method)
 * - Simplified method names based on return types
 * - Support for composite keys (PID, TID, NodeID)
 *
 * Follows initialization pattern:
 * 1. Constructor: Initialize internal variables (no allocation, no file
 * loading)
 * 2. initialize(): Initialize state and prepare for data
 * 3. cleanup(): Deallocate memory and clean up state
 * 4. Destructor: Clear all state
 */
class CallTree {
   public:
    /**
     * Default constructor for empty call graph
     * Only initializes internal variables to defaults
     */
    CallTree();

    /**
     * Construct call graph (note: does not load data, call initialize/load
     * separately)
     * @param log_file Path to trace log file (stored for later use)
     */
    explicit CallTree(const std::string& log_file);

    /**
     * Destructor - clears all state
     */
    ~CallTree();

    /**
     * Initialize the call graph state and factory
     * Must be called before adding data
     */
    void initialize();

    /**
     * Cleanup - deallocates all memory and cleans up state
     * Call at the end to ensure no memory leaks
     */
    void cleanup();

    /**
     * Get call graph for specific process/thread/node
     * Simplified name - return type tells the story
     */
    ProcessCallTree* get(const ProcessKey& key);

    /**
     * Convenience overload for get with separate parameters
     */
    ProcessCallTree* get(std::uint32_t pid, std::uint32_t tid = 0,
                         std::uint32_t node_id = 0);

    /**
     * Operator overload for natural C++ access
     */
    ProcessCallTree& operator[](const ProcessKey& key);

    /**
     * Get all process keys in the call graph
     * Renamed from get_process_ids to reflect composite key
     */
    std::vector<ProcessKey> keys() const;

    /**
     * Print call graph for specific process/thread/node
     * Simplified name
     */
    void print(const ProcessKey& key) const;

    /**
     * Convenience overload for print with separate parameters
     */
    void print(std::uint32_t pid, std::uint32_t tid = 0,
               std::uint32_t node_id = 0) const;

    /**
     * Check if call graph is empty
     */
    bool empty() const { return process_graphs_.empty(); }

    /**
     * Get number of process/thread/node combinations
     */
    size_t size() const { return process_graphs_.size(); }

    /**
     * Add a function call to the appropriate process graph
     * Used by TraceReader to populate the graph
     */
    void add_call(const ProcessKey& key, std::shared_ptr<CallTreeNode> call);

    // Moves every ProcessCallTree out of `other` into this tree. When both
    // sides share a ProcessKey, calls/call_sequence from `other` are appended.
    // `other` is left empty; intended for joining per-file CallTree fragments
    // built concurrently into a single merged tree.
    void merge_from(CallTree&& other);

    /**
     * Build parent-child relationships after all traces loaded
     * Called by TraceReader after all data is loaded
     */
    void build_hierarchy();

    /**
     * Build hierarchy for a specific process (lazy/on-demand)
     * @param key ProcessKey to build hierarchy for
     */
    void build_hierarchy_for_process(const ProcessKey& key);

    /**
     * Get the factory for creating nodes
     */
    CallTreeFactory& get_factory() { return factory_; }

   private:
    friend class TraceReader;
    std::unordered_map<ProcessKey, std::unique_ptr<ProcessCallTree>>
        process_graphs_;
    CallTreeFactory factory_;
    std::string log_file_;
    bool initialized_;
    bool cleaned_up_;

    /**
     * Load call graph from trace file (moved to private)
     * Delegates to TraceReader for actual I/O
     */
    bool load(const std::string& trace_file);

    /**
     * Build hierarchy for a single ProcessCallTree
     */
    void build_hierarchy_internal(ProcessCallTree* graph);

    /**
     * Print calls recursively
     */
    void print_calls_recursive(const ProcessCallTree& graph,
                               std::uint64_t call_id, int indent) const;
};

}  // namespace internal
}  // namespace dftracer::utils::call_tree

#endif  // DFTRACER_UTILS_CALL_TREE_INTERNAL_CALL_TREE_H
