#ifndef DFTRACER_UTILS_CORE_DISTRIBUTED_MPI_TRANSPORT_H
#define DFTRACER_UTILS_CORE_DISTRIBUTED_MPI_TRANSPORT_H

// MPI implementation of Transport. Safe to include anywhere: the body compiles
// only when the build enabled MPI (DFTRACER_UTILS_ENABLE_MPI, from config.h),
// so callers guard their MpiTransport use with the same macro. The MPI-free
// interface in transport.h keeps core buildable without MPI.

#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/distributed/transport.h>

#ifdef DFTRACER_UTILS_ENABLE_MPI

#include <mpi.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace dftracer::utils::distributed {

/// Transport backed by MPI collectives over `comm` (default MPI_COMM_WORLD).
class MpiTransport : public Transport {
   public:
    explicit MpiTransport(MPI_Comm comm = MPI_COMM_WORLD) : comm_(comm) {
        MPI_Comm_rank(comm_, &rank_);
        MPI_Comm_size(comm_, &size_);
    }
    int rank() const override { return rank_; }
    int size() const override { return size_; }
    void barrier() override { MPI_Barrier(comm_); }

    std::vector<std::string> all_gather(const std::string& payload) override {
        const int my_bytes = static_cast<int>(payload.size());
        std::vector<int> sizes(size_, 0);
        MPI_Allgather(&my_bytes, 1, MPI_INT, sizes.data(), 1, MPI_INT, comm_);
        std::vector<int> displs(size_, 0);
        long total = 0;
        for (int r = 0; r < size_; ++r) {
            displs[r] = static_cast<int>(total);
            total += sizes[r];
        }
        std::vector<char> buf(static_cast<std::size_t>(total));
        MPI_Allgatherv(payload.data(), my_bytes, MPI_CHAR, buf.data(),
                       sizes.data(), displs.data(), MPI_CHAR, comm_);
        std::vector<std::string> out(size_);
        for (int r = 0; r < size_; ++r)
            out[r].assign(buf.data() + displs[r],
                          static_cast<std::size_t>(sizes[r]));
        return out;
    }

   private:
    MPI_Comm comm_;
    int rank_ = 0;
    int size_ = 1;
};

/// Cap this rank's worker threads to its node-local share of cores so N ranks
/// on a node do not each spawn `cores` threads and oversubscribe (ranks on
/// separate nodes are independent). Applied via DFTRACER_UTILS_THREADS, so call
/// it before any runtime work. This is Dask's per-node worker/thread split for
/// MPI.
inline void set_local_thread_budget(MPI_Comm comm = MPI_COMM_WORLD) {
    int rank = 0;
    MPI_Comm_rank(comm, &rank);
    MPI_Comm local = MPI_COMM_NULL;
    MPI_Comm_split_type(comm, MPI_COMM_TYPE_SHARED, rank, MPI_INFO_NULL,
                        &local);
    int local_size = 1;
    MPI_Comm_size(local, &local_size);
    MPI_Comm_free(&local);

    const auto cores = static_cast<unsigned>(hardware_concurrency());
    unsigned per_rank =
        std::max(1u, cores / static_cast<unsigned>(std::max(1, local_size)));
    const std::string n = std::to_string(per_rank);
    ::setenv("DFTRACER_UTILS_THREADS", n.c_str(), 1);
    ::setenv("DFTRACER_UTILS_IO_THREADS", n.c_str(), 1);
}

}  // namespace dftracer::utils::distributed

#endif  // DFTRACER_UTILS_ENABLE_MPI

#endif  // DFTRACER_UTILS_CORE_DISTRIBUTED_MPI_TRANSPORT_H
