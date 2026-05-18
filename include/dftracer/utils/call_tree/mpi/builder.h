#ifndef DFTRACER_UTILS_CALL_TREE_MPI_BUILDER_H
#define DFTRACER_UTILS_CALL_TREE_MPI_BUILDER_H

#include <dftracer/utils/call_tree/internal/call_tree.h>
#include <dftracer/utils/call_tree/internal/process_call_tree.h>
#include <dftracer/utils/call_tree/internal/process_key.h>
#include <dftracer/utils/call_tree/mpi/config.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace dftracer::utils::call_tree {

// MPI-parallel call tree engine. Each method is a coroutine driven by the
// caller's pipeline; this class owns no Pipeline of its own. Phases follow
// the dftracer_aggregator_mpi pattern:
//
//   discover_pids_async : cooperative PID pre-scan + allgather + round-robin
//                         assign so each pid is owned by exactly one rank.
//   build_async         : per-file CoroScope fan-out, pid-filtered ingest,
//                         merge of per-file fragments into a local tree.
//   hierarchy_async     : per-process CoroScope fan-out (each PID lives on
//                         one rank so no cross-rank dependency).
//   write_async         : per-rank Chrome Tracing JSON shard via sharded
//                         ParallelWriter (io_backend driven).
//   merge_async         : rank 0 concatenates shards into the final output
//                         via fileio::parallel::merge_shards.
class MPICallTreeBuilder {
   public:
    explicit MPICallTreeBuilder(const MPICallTreeConfig& config);
    ~MPICallTreeBuilder();

    MPICallTreeBuilder(const MPICallTreeBuilder&) = delete;
    MPICallTreeBuilder& operator=(const MPICallTreeBuilder&) = delete;
    MPICallTreeBuilder(MPICallTreeBuilder&&) noexcept;
    MPICallTreeBuilder& operator=(MPICallTreeBuilder&&) noexcept;

    void add_trace_files(const std::vector<std::string>& files);
    void add_trace_directory(const std::string& directory,
                             const std::string& pattern = "*.pfw.gz");

    coro::CoroTask<bool> discover_pids(CoroScope* scope);
    coro::CoroTask<bool> build(CoroScope* scope);
    coro::CoroTask<bool> hierarchy(CoroScope* scope);
    coro::CoroTask<bool> write(CoroScope* scope, std::string output_path,
                               std::string staging_dir, bool gzip);
    coro::CoroTask<bool> merge(std::string output_path, std::string staging_dir,
                               bool gzip, bool keep_staging);

    int rank() const { return rank_; }
    int world_size() const { return world_size_; }

    const std::vector<std::string>& trace_files() const { return trace_files_; }
    const std::set<std::uint32_t>& all_pids() const { return all_pids_; }
    const std::set<std::uint32_t>& assigned_pids() const {
        return assigned_pids_;
    }
    internal::CallTree& local_tree() { return *call_tree_; }
    const internal::CallTree& local_tree() const { return *call_tree_; }

   private:
    MPICallTreeConfig config_;
    std::unique_ptr<internal::CallTree> call_tree_;

    int rank_ = 0;
    int world_size_ = 1;

    std::vector<std::string> trace_files_;
    std::set<std::uint32_t> all_pids_;
    std::set<std::uint32_t> assigned_pids_;
    std::vector<internal::ProcessKey> my_process_keys_;

    std::string my_shard_path_;
};

}  // namespace dftracer::utils::call_tree

#endif  // DFTRACER_UTILS_CALL_TREE_MPI_BUILDER_H
