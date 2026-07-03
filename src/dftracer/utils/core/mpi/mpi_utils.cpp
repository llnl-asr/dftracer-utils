#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/mpi/mpi_utils.h>

namespace dftracer::utils::mpi {

namespace {

#ifdef DFTRACER_UTILS_MPI_ENABLED
// Prefix-sum recv_counts into displacements; returns the total element count.
int compute_displacements(const std::vector<int>& recv_counts, int world_size,
                          std::vector<int>& displacements) {
    displacements.resize(world_size);
    int total = 0;
    for (int i = 0; i < world_size; i++) {
        displacements[i] = total;
        total += recv_counts[i];
    }
    return total;
}
#endif

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

bool MPIUtils::is_mpi_enabled() const {
#ifdef DFTRACER_UTILS_MPI_ENABLED
    return true;
#else
    return false;
#endif
}

void MPIUtils::barrier() {
#ifdef DFTRACER_UTILS_MPI_ENABLED
    if (initialized_) {
        MPI_Barrier(MPI_COMM_WORLD);
    }
#endif
}

void MPIUtils::broadcast_string(std::string& str, int root) {
#ifdef DFTRACER_UTILS_MPI_ENABLED
    if (!initialized_) return;

    int len = static_cast<int>(str.size());
    MPI_Bcast(&len, 1, MPI_INT, root, MPI_COMM_WORLD);

    if (rank_ != root) {
        str.resize(len);
    }

    if (len > 0) {
        MPI_Bcast(&str[0], len, MPI_CHAR, root, MPI_COMM_WORLD);
    }
#else
    (void)str;
    (void)root;
#endif
}

void MPIUtils::broadcast_uint32_vector(std::vector<std::uint32_t>& values,
                                       int root) {
#ifdef DFTRACER_UTILS_MPI_ENABLED
    if (!initialized_) return;

    int count = static_cast<int>(values.size());
    MPI_Bcast(&count, 1, MPI_INT, root, MPI_COMM_WORLD);

    if (rank_ != root) {
        values.resize(count);
    }

    if (count > 0) {
        MPI_Bcast(values.data(), count, MPI_UINT32_T, root, MPI_COMM_WORLD);
    }
#else
    (void)values;
    (void)root;
#endif
}

void MPIUtils::broadcast_int(int& value, int root) {
#ifdef DFTRACER_UTILS_MPI_ENABLED
    if (!initialized_) return;

    MPI_Bcast(&value, 1, MPI_INT, root, MPI_COMM_WORLD);
#else
    (void)value;
    (void)root;
#endif
}

void MPIUtils::gather_int(int send_value, std::vector<int>& recv_values,
                          int root) {
#ifdef DFTRACER_UTILS_MPI_ENABLED
    if (!initialized_) return;

    if (rank_ == root) {
        recv_values.resize(world_size_);
    }

    MPI_Gather(&send_value, 1, MPI_INT, recv_values.data(), 1, MPI_INT, root,
               MPI_COMM_WORLD);
#else
    recv_values.clear();
    recv_values.push_back(send_value);
    (void)root;
#endif
}

void MPIUtils::gatherv_uint32(const std::vector<std::uint32_t>& send_data,
                              std::vector<std::uint32_t>& recv_data,
                              std::vector<int>& recv_counts,
                              std::vector<int>& displacements, int root) {
#ifdef DFTRACER_UTILS_MPI_ENABLED
    if (!initialized_) return;

    int local_count = static_cast<int>(send_data.size());

    // Gather counts from all ranks to root
    if (rank_ == root) {
        recv_counts.resize(world_size_);
    }
    MPI_Gather(&local_count, 1, MPI_INT, recv_counts.data(), 1, MPI_INT, root,
               MPI_COMM_WORLD);

    // Calculate displacements and total size on root
    if (rank_ == root) {
        int total =
            compute_displacements(recv_counts, world_size_, displacements);
        recv_data.resize(total);
    }

    MPI_Gatherv(send_data.data(), local_count, MPI_UINT32_T, recv_data.data(),
                recv_counts.data(), displacements.data(), MPI_UINT32_T, root,
                MPI_COMM_WORLD);
#else
    serial_gatherv_fallback(send_data, recv_data, recv_counts, displacements);
    (void)root;
#endif
}

void MPIUtils::allgather_int(int send_value, std::vector<int>& recv_values) {
#ifdef DFTRACER_UTILS_MPI_ENABLED
    if (!initialized_) {
        recv_values.clear();
        recv_values.push_back(send_value);
        return;
    }

    recv_values.resize(world_size_);
    MPI_Allgather(&send_value, 1, MPI_INT, recv_values.data(), 1, MPI_INT,
                  MPI_COMM_WORLD);
#else
    recv_values.clear();
    recv_values.push_back(send_value);
#endif
}

void MPIUtils::allgatherv_char(const std::vector<char>& send_data,
                               std::vector<char>& recv_data,
                               std::vector<int>& recv_sizes,
                               std::vector<int>& displacements) {
#ifdef DFTRACER_UTILS_MPI_ENABLED
    if (!initialized_) {
        serial_gatherv_fallback(send_data, recv_data, recv_sizes,
                                displacements);
        return;
    }

    int send_size = static_cast<int>(send_data.size());
    recv_sizes.resize(world_size_);
    MPI_Allgather(&send_size, 1, MPI_INT, recv_sizes.data(), 1, MPI_INT,
                  MPI_COMM_WORLD);

    int total_recv =
        compute_displacements(recv_sizes, world_size_, displacements);
    recv_data.resize(total_recv);
    MPI_Allgatherv(send_data.data(), send_size, MPI_CHAR, recv_data.data(),
                   recv_sizes.data(), displacements.data(), MPI_CHAR,
                   MPI_COMM_WORLD);
#else
    serial_gatherv_fallback(send_data, recv_data, recv_sizes, displacements);
#endif
}

void MPIUtils::reduce_sum_size_t(std::size_t send_value,
                                 std::size_t& recv_value, int root) {
#ifdef DFTRACER_UTILS_MPI_ENABLED
    if (!initialized_) {
        recv_value = send_value;
        return;
    }

    MPI_Reduce(&send_value, &recv_value, 1, MPI_UNSIGNED_LONG, MPI_SUM, root,
               MPI_COMM_WORLD);
#else
    recv_value = send_value;
    (void)root;
#endif
}

void MPIUtils::reduce_max_double(double send_value, double& recv_value,
                                 int root) {
#ifdef DFTRACER_UTILS_MPI_ENABLED
    if (!initialized_) {
        recv_value = send_value;
        return;
    }

    MPI_Reduce(&send_value, &recv_value, 1, MPI_DOUBLE, MPI_MAX, root,
               MPI_COMM_WORLD);
#else
    recv_value = send_value;
    (void)root;
#endif
}

}  // namespace dftracer::utils::mpi
