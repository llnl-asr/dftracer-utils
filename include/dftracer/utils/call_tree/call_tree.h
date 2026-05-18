#ifndef DFTRACER_UTILS_CALL_TREE_H
#define DFTRACER_UTILS_CALL_TREE_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::call_tree {

/**
 * Simple node information structure for public API
 * Contains basic information about a node without complex internals
 */
struct CallTreeNodeInfo {
    std::uint64_t id;
    std::string name;
    std::string category;
    std::uint64_t start_time_us;
    std::uint64_t duration_us;
    int level;
    std::uint64_t parent_id;
    size_t num_children;
    std::vector<std::uint64_t> children_ids;  // IDs of child nodes
    std::unordered_map<std::string, std::string>
        args;  // Node arguments (pid, tid, fhash, etc.)

    CallTreeNodeInfo()
        : id(0),
          name(""),
          category(""),
          start_time_us(0),
          duration_us(0),
          level(0),
          parent_id(0),
          num_children(0) {}
};

/**
 * Aggregate statistics about the call tree
 */
struct CallTreeStats {
    size_t total_nodes;
    size_t num_levels;
    size_t num_leaf_nodes;
    size_t num_processes;
    int max_depth;  // Maximum depth of the tree (alias for num_levels - 1)
    size_t unique_processes;  // Unique process count (alias for num_processes)
    std::vector<double>
        avg_time_per_level_us;  // Average time in microseconds per level
    std::vector<size_t> nodes_per_level;  // Number of nodes per level

    CallTreeStats()
        : total_nodes(0),
          num_levels(0),
          num_leaf_nodes(0),
          num_processes(0),
          max_depth(0),
          unique_processes(0) {}
};

// Forward declarations
namespace internal {
class CallTree;
class CallTreeImpl;
}  // namespace internal

/**
 * @brief Simple, clean API for working with call trees from DFTracer traces.
 *
 * Usage:
 * @code
 *   CallTree tree;
 *   tree.load_from_directory("/path/to/traces");
 *   tree.generate();
 *   tree.print_depth_first();
 *   auto nodes = tree.get_nodes_depth_first();
 *   auto stats = tree.get_statistics();
 *   tree.save_to_file("output.calltree");
 * @endcode
 */
class CallTree {
   public:
    /**
     * Constructor
     */
    CallTree();

    /**
     * Destructor
     */
    ~CallTree();

    // Disable copy, enable move
    CallTree(const CallTree&) = delete;
    CallTree& operator=(const CallTree&) = delete;
    CallTree(CallTree&&) noexcept;
    CallTree& operator=(CallTree&&) noexcept;

    /**
     * Specify trace directory path
     * Automatically finds all .gz compressed trace files in the directory
     * @param trace_dir Path to directory containing trace files
     * @param pattern Optional file pattern (default: "*.pfw.gz")
     * @return true if directory exists and files found, false otherwise
     */
    bool load_from_directory(const std::string& trace_dir,
                             const std::string& pattern = "*.pfw.gz");

    /**
     * Generate call tree from loaded traces
     * Reads all trace files and builds the in-memory call tree structure
     * @return true if successful, false otherwise
     */
    bool generate();

    /// Print depth-first tree to stdout. max_depth=0 means unlimited.
    void print_depth_first(int max_depth = 0) const;

    std::vector<CallTreeNodeInfo> get_nodes_depth_first() const;

    /**
     * Get aggregate statistics about the call tree
     * @return Statistics structure with aggregate information
     */
    CallTreeStats get_statistics() const;

    /**
     * Print aggregate statistics to stdout
     */
    void print_statistics() const;

    /**
     * Check if call tree has been generated
     * @return true if generate() completed successfully
     */
    bool is_generated() const;

    /**
     * Get number of trace files loaded
     * @return Number of trace files
     */
    size_t get_num_trace_files() const;

    /**
     * Clear all data and reset to initial state
     */
    void clear();

    /**
     * Get all unique process IDs in the call tree
     * @return Vector of process IDs
     */
    std::vector<std::uint32_t> get_process_ids() const;

    /**
     * Get all thread IDs for a specific process
     * @param pid Process ID
     * @return Vector of thread IDs
     */
    std::vector<std::uint32_t> get_thread_ids(std::uint32_t pid) const;

    /**
     * Get root nodes for a specific process/thread combination
     * @param pid Process ID
     * @param tid Thread ID
     * @return Vector of root node information
     */
    std::vector<CallTreeNodeInfo> get_root_nodes(std::uint32_t pid,
                                                 std::uint32_t tid) const;

    /**
     * Get all nodes in the tree
     * @return Vector of all node information
     */
    std::vector<CallTreeNodeInfo> get_all_nodes() const;

    /**
     * Get a specific node by ID
     * @param id Node ID
     * @return Node information (empty if not found)
     */
    CallTreeNodeInfo get_node_by_id(std::uint64_t id) const;

    /// Direct access to the underlying internal::CallTree. Use with the
    /// save_binary / save_arrow coroutines in mpi/serializable.h. Returns a
    /// reference; callers must keep the CallTree alive while it's in use.
    internal::CallTree& internal_tree();
    const internal::CallTree& internal_tree() const;

   private:
    std::unique_ptr<internal::CallTreeImpl> impl_;
};

}  // namespace dftracer::utils::call_tree

#endif  // DFTRACER_UTILS_CALL_TREE_H
