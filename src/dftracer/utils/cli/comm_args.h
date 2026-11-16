// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_CLI_COMM_ARGS_H
#define DFTRACER_UTILS_CLI_COMM_ARGS_H

#include <dftracer/utils/cli/common_cli.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::cli {

/// The cores rank @p rank may use of its node, given the CPU mask of every
/// rank of the node as words of 64 CPUs. Ranks that all see the whole node
/// split it, and ranks a launcher bound to their own CPUs keep them: the share
/// is the CPUs of the rank times the CPUs of every mask together over the sum
/// of the masks, at least 1 and at most @p limit, which is the core count of
/// this process after DFTRACER_UTILS_HW_CONCURRENCY.
std::size_t core_share(dstd::Span<const dstd::Vector<std::uint64_t>> masks,
                       std::size_t rank, std::size_t limit);

/// The communicator of a tool. `--comm local|mpi|auto` picks the backend, and
/// DFTRACER_UTILS_COMM does the same when the option is absent; auto is MPI
/// under a launcher and one local rank otherwise. A run with no launcher is a
/// group of one rank, so a tool calls @ref world and @ref node whatever the
/// backend is. The communicator starts after the parse, so a help or a parse
/// error starts no MPI.
///
/// When several ranks share a node, @ref after_parse also gives each rank its
/// share of the node's cores through DFTRACER_UTILS_THREADS and
/// DFTRACER_UTILS_IO_THREADS, which win over --executor-threads and
/// --io-threads.
struct CommArgs : CliSchema {
    void register_on(ArgParser& p) override;
    dstd::Result<void> after_parse() override;

    /// Every rank of the run; valid once the parse ended.
    comm::Group world() const noexcept { return world_; }

    /// The ranks that share this node; valid once the parse ended.
    comm::Group node() const noexcept { return node_; }

   private:
    dstd::StringView backend_;
    dstd::Optional<comm::Communicator> communicator_;
    comm::Group world_;
    comm::Group node_;
};

}  // namespace dftracer::utils::cli

#endif  // DFTRACER_UTILS_CLI_COMM_ARGS_H
