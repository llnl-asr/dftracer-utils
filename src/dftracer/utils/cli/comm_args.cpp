// SPDX-License-Identifier: MIT
#include <dftracer/utils/cli/comm_args.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/to_chars.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <sched.h>
#endif

namespace dftracer::utils::cli {

namespace {

dstd::Result<comm::BackendKind> backend_kind(dstd::StringView name) {
    if (name.empty() || name == "auto") return comm::BackendKind::Auto;
    if (name == "local") return comm::BackendKind::Local;
    if (name == "mpi") return comm::BackendKind::Mpi;
    return dstd::Unexpected(
        dstd::Error::from_format<"--comm takes local, mpi or auto, not '{}'">(
            dstd::Condition::InvalidArgument, name));
}

#if defined(__linux__)
constexpr std::size_t MASK_WORDS = CPU_SETSIZE / 64;

// The CPUs this process may run on; when the kernel will not say, the CPUs
// online. A rank always contributes a mask, so its peers never wait for it.
dstd::Vector<std::uint64_t> own_mask() {
    dstd::Vector<std::uint64_t> words(MASK_WORDS, 0);
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (std::size_t cpu = 0; cpu < MASK_WORDS * 64; ++cpu)
            if (CPU_ISSET(cpu, &set))
                words[cpu / 64] |= std::uint64_t{1} << (cpu % 64);
        return words;
    }
    const std::size_t online = std::min<std::size_t>(
        MASK_WORDS * 64, dstd::Thread::hardware_concurrency());
    for (std::size_t cpu = 0; cpu < online; ++cpu)
        words[cpu / 64] |= std::uint64_t{1} << (cpu % 64);
    return words;
}

dstd::Result<std::size_t> node_share(comm::Group& node) {
    const dstd::Vector<std::uint64_t> mine = own_mask();
    auto op = node.all_gather<std::uint64_t>(
        dstd::Span<const std::uint64_t>(mine.data(), mine.size()));
    auto gathered = op.wait();
    if (!gathered) return dstd::Unexpected(gathered.error());
    dstd::Vector<dstd::Vector<std::uint64_t>> masks;
    for (std::uint32_t rank = 0; rank < gathered->size(); ++rank) {
        const dstd::Span<const std::byte> bytes = (*gathered)[rank];
        dstd::Vector<std::uint64_t> words(bytes.size() / sizeof(std::uint64_t));
        if (!words.empty())
            std::memcpy(words.data(), bytes.data(),
                        words.size() * sizeof(std::uint64_t));
        masks.push_back(std::move(words));
    }
    return core_share(dstd::Span<const dstd::Vector<std::uint64_t>>(
                          masks.data(), masks.size()),
                      node.rank(), common::hardware_concurrency());
}
#else
dstd::Result<std::size_t> node_share(comm::Group& node) {
    return common::threads_per_local_rank(node.size());
}
#endif

}  // namespace

std::size_t core_share(dstd::Span<const dstd::Vector<std::uint64_t>> masks,
                       std::size_t rank, std::size_t limit) {
    std::size_t words = 0;
    for (const auto& mask : masks) words = std::max(words, mask.size());
    dstd::Vector<std::uint64_t> joined(words, 0);
    std::size_t total = 0;
    std::size_t own = 0;
    for (std::size_t at = 0; at < masks.size(); ++at) {
        std::size_t count = 0;
        for (std::size_t word = 0; word < masks[at].size(); ++word) {
            joined[word] |= masks[at][word];
            count += static_cast<std::size_t>(std::popcount(masks[at][word]));
        }
        total += count;
        if (at == rank) own = count;
    }
    std::size_t all = 0;
    for (const std::uint64_t word : joined)
        all += static_cast<std::size_t>(std::popcount(word));
    if (total == 0) return 1;
    return std::min(std::max<std::size_t>(1, own * all / total),
                    std::max<std::size_t>(1, limit));
}

void CommArgs::register_on(ArgParser& p) {
    p.group("Distribution");
    p.option(backend_, {"--comm"},
             "Backend of the ranks: local (one rank), mpi (one rank per "
             "process, from a launcher) or auto, which is mpi under a "
             "launcher and local otherwise (default: auto, or "
             "DFTRACER_UTILS_COMM)");
}

dstd::Result<void> CommArgs::after_parse() {
    dstd::StringView name = backend_;
    if (name.empty()) {
        if (const auto from_env = common::Env::get<dstd::StringView>(
                common::constants::comm_settings::COMM_ENV))
            name = *from_env;
    }
    auto kind = backend_kind(name);
    if (!kind) return dstd::Unexpected(kind.error());
    auto started = comm::Communicator::start(*kind);
    if (!started) return dstd::Unexpected(started.error());
    communicator_.emplace(std::move(*started));
    world_ = communicator_->world(communicator_->rank());
    node_ = communicator_->local(communicator_->rank());
    if (node_.size() <= 1) return {};
    auto share = node_share(node_);
    if (!share) return dstd::Unexpected(share.error());
    const dstd::String threads = dstd::to_string(*share);
    ::setenv("DFTRACER_UTILS_THREADS", threads.c_str(), 1);
    ::setenv("DFTRACER_UTILS_IO_THREADS", threads.c_str(), 1);
    return {};
}

}  // namespace dftracer::utils::cli
