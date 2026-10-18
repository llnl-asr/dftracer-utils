#ifndef DFTRACER_UTILS_RUNTIME_MPI_UTILS_H
#define DFTRACER_UTILS_RUNTIME_MPI_UTILS_H

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

    /// Delete copy and move constructors/operators
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
     * Check if MPI is enabled (compiled with MPI support)
     */
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

#endif  // DFTRACER_UTILS_RUNTIME_MPI_UTILS_H
