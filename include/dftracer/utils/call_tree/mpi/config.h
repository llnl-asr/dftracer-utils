#ifndef DFTRACER_UTILS_CALL_TREE_MPI_CONFIG_H
#define DFTRACER_UTILS_CALL_TREE_MPI_CONFIG_H

/**
 * @file config.h
 * @brief Configuration and result structures for MPI call tree generation
 */

#include <cstdint>
#include <string>

namespace dftracer::utils::call_tree {

/**
 * Configuration for MPI call tree generation
 */
struct MPICallTreeConfig {
    std::string output_file;                // Output file for call tree
    std::string file_pattern = "*.pfw.gz";  // Pattern for trace files
    bool use_indexer = true;                // Use indexer for gzip files
    bool summary_only = false;              // Only print summary
    std::size_t num_threads = 0;            // Threads for pipeline (0 = auto)
    std::uint64_t checkpoint_size = 0;  // Indexer checkpoint size (0 = default)
};

}  // namespace dftracer::utils::call_tree

#endif  // DFTRACER_UTILS_CALL_TREE_MPI_CONFIG_H
