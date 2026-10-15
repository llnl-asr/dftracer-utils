#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/core/mpi/mpi_utils.h>

namespace dftracer::utils::mpi {

namespace {

// Single-rank fallback: the gathered result is just the local send buffer.
template <typename T>
void serial_gatherv_fallback(const std::vector<T>& send_data,
                             std::vector<T>& recv_data,
                             std::vector<int>& recv_counts,
                             std::vector<int>& displacements) {
    recv_data = send_data;
    recv_counts.clear();
    recv_counts.push_back(static_cast<int>(send_data.size()));
    displacements.clear();
    displacements.push_back(0);
}

}  // namespace

MPIUtils::MPIUtils() : rank_(0), world_size_(1), initialized_(false) {}

MPIUtils::~MPIUtils() {
    // Don't call finalize here - let the user control that
}

MPIUtils& MPIUtils::instance() {
    static MPIUtils instance;
    return instance;
}

bool MPIUtils::initialize() {
    if (initialized_) {
        return true;
    }

#ifdef DFTRACER_UTILS_MPI_ENABLED
    int mpi_init = 0;
    MPI_Initialized(&mpi_init);
    if (mpi_init) {
        MPI_Comm_rank(MPI_COMM_WORLD, &rank_);
        MPI_Comm_size(MPI_COMM_WORLD, &world_size_);
        initialized_ = true;
        return true;
    }
    // MPI not initialized yet - return false but don't log error
    // The caller may initialize MPI later
    return false;
#else
    // No MPI support compiled in - use defaults (rank 0, size 1)
    initialized_ = true;
    return true;
#endif
}

void MPIUtils::finalize() {
    // Reset state but don't call MPI_Finalize
    initialized_ = false;
    rank_ = 0;
    world_size_ = 1;
}

void MPIUtils::barrier() {
#ifdef DFTRACER_UTILS_MPI_ENABLED
    if (initialized_) {
        MPI_Barrier(MPI_COMM_WORLD);
    }
#endif
}

}  // namespace dftracer::utils::mpi
