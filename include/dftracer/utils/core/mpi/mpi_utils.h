#ifndef DFTRACER_UTILS_CORE_MPI_UTILS_H
#define DFTRACER_UTILS_CORE_MPI_UTILS_H

/**
 * @file mpi_utils.h
 * @brief Singleton class for MPI utilities
 *
 * Provides a global singleton for MPI initialization and common operations.
 * This avoids duplication of MPI rank/size queries and ensures consistent
 * MPI state across all components.
 */

#include <cstdint>
#include <string>
#include <vector>

#ifdef DFTRACER_UTILS_MPI_ENABLED
#include <mpi.h>
#endif

namespace dftracer::utils::mpi {

/**
 * MPIUtils - Singleton class for MPI utilities
 *
 * Usage:
 * @code
 *   // Initialize (call once after MPI_Init)
 *   MPIUtils::instance().initialize();
 * @endcode
 *
 *   // Use throughout application
 *   int rank = MPIUtils::instance().get_rank();
 *   int size = MPIUtils::instance().get_world_size();
 *
 *   // Cleanup (call before MPI_Finalize if needed)
 *   MPIUtils::instance().finalize();
 */
class MPIUtils {
   public:
    /**
     * Get the singleton instance
     */
    static MPIUtils& instance();

    // Delete copy and move constructors/operators
    MPIUtils(const MPIUtils&) = delete;
    MPIUtils& operator=(const MPIUtils&) = delete;
    MPIUtils(MPIUtils&&) = delete;
    MPIUtils& operator=(MPIUtils&&) = delete;

    /**
     * Initialize MPI utilities
     * Must be called after MPI_Init
     * @return true if MPI is available and initialized
     */
    bool initialize();

    /**
     * Finalize MPI utilities (cleanup internal state)
     * Does NOT call MPI_Finalize - that's the caller's responsibility
     */
    void finalize();

    /**
     * Check if MPI is initialized and available
     */
    bool is_initialized() const { return initialized_; }

    /**
     * Check if MPI is enabled (compiled with MPI support)
     */
    bool is_mpi_enabled() const;

    /**
     * Get MPI rank (0 if MPI not initialized)
     */
    int get_rank() const { return rank_; }

    /**
     * Get MPI world size (1 if MPI not initialized)
     */
    int get_world_size() const { return world_size_; }

    /**
     * Check if this is the root rank (rank 0)
     */
    bool is_root() const { return rank_ == 0; }

    // =========================================================================
    // Collective operations
    // =========================================================================

    /**
     * Barrier - synchronize all ranks
     */
    void barrier();

    /**
     * Broadcast a string from root to all ranks
     * @param str String to broadcast (modified on non-root ranks)
     * @param root Root rank (default 0)
     */
    void broadcast_string(std::string& str, int root = 0);

    /**
     * Broadcast a vector of uint32_t from root to all ranks
     * @param values Vector to broadcast (modified on non-root ranks)
     * @param root Root rank (default 0)
     */
    void broadcast_uint32_vector(std::vector<std::uint32_t>& values,
                                 int root = 0);

    /**
     * Broadcast a single integer from root to all ranks
     * @param value Integer to broadcast (modified on non-root ranks)
     * @param root Root rank (default 0)
     */
    void broadcast_int(int& value, int root = 0);

    /**
     * Gather integers from all ranks to root
     * @param send_value Value to send from this rank
     * @param recv_values Vector to receive values (only valid on root)
     * @param root Root rank (default 0)
     */
    void gather_int(int send_value, std::vector<int>& recv_values,
                    int root = 0);

    /**
     * Gatherv - gather variable-sized data from all ranks to root
     * @param send_data Data to send from this rank
     * @param recv_data Buffer to receive data (only valid on root)
     * @param recv_counts Number of elements from each rank (only valid on root)
     * @param displacements Displacements for each rank (only valid on root)
     * @param root Root rank (default 0)
     */
    void gatherv_uint32(const std::vector<std::uint32_t>& send_data,
                        std::vector<std::uint32_t>& recv_data,
                        std::vector<int>& recv_counts,
                        std::vector<int>& displacements, int root = 0);

    /**
     * Allgather - gather data from all ranks to all ranks
     * @param send_value Value to send from this rank
     * @param recv_values Vector to receive all values (resized to world_size)
     */
    void allgather_int(int send_value, std::vector<int>& recv_values);

    /**
     * Allgatherv - gather variable-sized data from all ranks to all ranks
     * @param send_data Data to send from this rank
     * @param recv_data Buffer to receive all data
     * @param recv_sizes Number of elements from each rank
     * @param displacements Displacements for each rank
     */
    void allgatherv_char(const std::vector<char>& send_data,
                         std::vector<char>& recv_data,
                         std::vector<int>& recv_sizes,
                         std::vector<int>& displacements);

    /**
     * Reduce to root - sum operation
     * @param send_value Value to reduce from this rank
     * @param recv_value Result (only valid on root)
     * @param root Root rank (default 0)
     */
    void reduce_sum_size_t(std::size_t send_value, std::size_t& recv_value,
                           int root = 0);

    /**
     * Reduce to root - max operation for double
     * @param send_value Value to reduce from this rank
     * @param recv_value Result (only valid on root)
     * @param root Root rank (default 0)
     */
    void reduce_max_double(double send_value, double& recv_value, int root = 0);

   private:
    /**
     * Private constructor for singleton
     */
    MPIUtils();

    /**
     * Private destructor
     */
    ~MPIUtils();

    int rank_;
    int world_size_;
    bool initialized_;
};

}  // namespace dftracer::utils::mpi

#endif  // DFTRACER_UTILS_CORE_MPI_UTILS_H
