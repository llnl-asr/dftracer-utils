#ifndef DFTRACER_UTILS_CALL_TREE_INTERNAL_FACTORY_H
#define DFTRACER_UTILS_CALL_TREE_INTERNAL_FACTORY_H

#include <dftracer/utils/call_tree/internal/node.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace dftracer::utils::call_tree {
namespace internal {

/**
 * CallTreeFactory - Factory for creating and managing CallTreeNode objects
 * Follows initialization pattern:
 * 1. Constructor: Initialize internal variables (no allocation)
 * 2. initialize(): Initialize state and prepare for node creation
 * 3. cleanup(): Deallocate all nodes and clean up state
 * 4. Destructor: Clear all state
 */
class CallTreeFactory {
   public:
    /**
     * Constructor - initializes internal variables to defaults
     */
    CallTreeFactory();

    /**
     * Destructor - clears all state
     */
    ~CallTreeFactory();

    /**
     * Initialize the factory state
     */
    void initialize();

    /**
     * Cleanup - deallocates all managed nodes
     */
    void cleanup();

    /**
     * Create a new CallTreeNode from trace event data
     * The factory manages the lifecycle of created nodes
     */
    std::shared_ptr<CallTreeNode> create_node(std::uint64_t id,
                                              std::string_view name,
                                              std::string_view category,
                                              std::uint64_t start_time,
                                              std::uint64_t duration, int level,
                                              ArgsMap args = {});

    /**
     * Get total number of nodes created by this factory
     */
    size_t get_node_count() const { return node_count_; }

   private:
    size_t node_count_;
    bool initialized_;
    bool cleaned_up_;

    // Track all nodes for proper cleanup
    std::vector<std::shared_ptr<CallTreeNode>> managed_nodes_;
};

}  // namespace internal
}  // namespace dftracer::utils::call_tree

#endif  // DFTRACER_UTILS_CALL_TREE_INTERNAL_FACTORY_H
