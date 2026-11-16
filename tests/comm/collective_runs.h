// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_COMM_COLLECTIVE_RUNS_H
#define DFTRACER_UTILS_TESTS_COMM_COLLECTIVE_RUNS_H

// Helpers shared by the collective tests: the bindings of the collectives a
// communicator runs on, a rank runner per binding and a provider with one
// linear instance per rank.

#include <comm/support.h>
#include <dftracer/utils/comm/backend/bootstrap.h>
#include <dftracer/utils/comm/backend/linear_collectives.h>
#include <dftracer/utils/comm/backend/loader.h>
#include <dftracer/utils/comm/backend/local.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace dftu_comm_test {

/// Which collectives a communicator of the test runs on.
enum class Binding : std::uint8_t { Local, Linear };

constexpr Binding BINDINGS[] = {Binding::Local, Binding::Linear};
constexpr std::uint32_t RANK_COUNTS[] = {1, 2, 3, 4};

inline const char* binding_name(Binding binding) noexcept {
    return binding == Binding::Local
               ? "local collectives"
               : "linear collectives over local messaging";
}

/// The local bootstrap with the processes a test names, so a group of one test
/// process behaves as one of several.
class ProcessBootstrap : public dftracer::utils::comm::backend::Bootstrap {
   public:
    ProcessBootstrap(dftracer::utils::dstd::SharedPtr<
                         dftracer::utils::comm::backend::Bootstrap>
                         inner,
                     dftracer::utils::dstd::Vector<std::uint32_t> processes)
        : inner_(std::move(inner)), processes_(std::move(processes)) {}

    std::uint32_t ranks() const noexcept override { return inner_->ranks(); }

    std::uint32_t own_rank() const noexcept override {
        return inner_->own_rank();
    }

    std::uint32_t process_of(std::uint32_t rank) const noexcept override {
        return processes_[rank];
    }

    dftracer::utils::dstd::Vector<std::uint32_t> node_members(
        std::uint32_t rank) const override {
        return inner_->node_members(rank);
    }

    dftracer::utils::coro::TimerService* timers() const noexcept override {
        return inner_->timers();
    }

   private:
    dftracer::utils::dstd::SharedPtr<dftracer::utils::comm::backend::Bootstrap>
        inner_;
    dftracer::utils::dstd::Vector<std::uint32_t> processes_;
};

/// A process id per rank, each rank in a process of its own.
inline dftracer::utils::dstd::Vector<std::uint32_t> process_per_rank(
    std::uint32_t ranks) {
    dftracer::utils::dstd::Vector<std::uint32_t> processes(ranks);
    for (std::uint32_t rank = 0; rank < ranks; ++rank) processes[rank] = rank;
    return processes;
}

/// A communicator of @p ranks ranks that live in the processes @p processes
/// names, over local messaging and the collectives @p make builds from it.
template <class Make>
dftracer::utils::comm::Communicator start_processes(
    std::uint32_t ranks, dftracer::utils::coro::Executor& executor,
    dftracer::utils::dstd::Vector<std::uint32_t> processes, Make make) {
    auto messaging = dftracer::utils::comm::backend::make_local_messaging();
    auto collectives = make(messaging);
    auto started = dftracer::utils::comm::Communicator::start(
        dftracer::utils::comm::Providers{
            dftracer::utils::dstd::make_shared<ProcessBootstrap>(
                dftracer::utils::comm::backend::make_local_bootstrap(ranks,
                                                                     executor),
                std::move(processes)),
            std::move(messaging), std::move(collectives)});
    REQUIRE(started.has_value());
    return std::move(*started);
}

inline dftracer::utils::comm::Communicator start(
    std::uint32_t ranks, dftracer::utils::coro::Executor& executor,
    Binding binding) {
    using dftracer::utils::comm::backend::make_linear_collectives;
    using dftracer::utils::comm::backend::make_local_collectives;
    if (binding == Binding::Linear) {
        return start_processes(ranks, executor, process_per_rank(ranks),
                               [](const auto& messaging) {
                                   return make_linear_collectives(messaging);
                               });
    }
    return start_processes(
        ranks, executor, dftracer::utils::dstd::Vector<std::uint32_t>(ranks, 0),
        [](const auto&) { return make_local_collectives(); });
}

/// Runs @p body(runtime, communicator, ranks) for every binding and every rank
/// count.
template <class Body>
void for_each_run(Body body) {
    for (const Binding binding : BINDINGS) {
        for (const std::uint32_t ranks : RANK_COUNTS) {
            CAPTURE(binding_name(binding));
            CAPTURE(ranks);
            dftracer::utils::Runtime rt(4);
            dftracer::utils::comm::Communicator communicator =
                start(ranks, *rt.executor(), binding);
            body(rt, communicator, ranks);
        }
    }
}

inline dftracer::utils::dstd::Span<const std::byte> bytes_of(
    const std::string& text) {
    return std::as_bytes(
        dftracer::utils::dstd::Span<const char>(text.data(), text.size()));
}

inline std::string text_of(dftracer::utils::dstd::Span<const std::byte> bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()),
                       bytes.size());
}

/// One linear provider per rank over one messaging, as separate processes have
/// them. Only the Done of @p driver ends the calls of the group, which a test
/// shares in one process, so the table that rank parsed is the one that counts.
class PerRank : public dftracer::utils::comm::backend::Collectives {
   public:
    PerRank(dftracer::utils::dstd::SharedPtr<
                dftracer::utils::comm::backend::Messaging>
                messaging,
            std::uint32_t ranks, std::uint32_t driver_rank)
        : driver_(driver_rank) {
        for (std::uint32_t rank = 0; rank < ranks; ++rank)
            instances_.push_back(
                dftracer::utils::comm::backend::make_linear_collectives(
                    messaging));
    }

    bool arrive(std::uint64_t group_id, std::uint64_t seq,
                std::uint32_t members, std::uint32_t rank,
                const dftracer::utils::comm::backend::Record& record,
                dftracer::utils::comm::backend::Contribution&& contribution,
                Done done) override {
        if (rank != driver_)
            done = [](const dftracer::utils::comm::backend::SlotEnd&) {};
        return instances_[rank]->arrive(group_id, seq, members, rank, record,
                                        std::move(contribution),
                                        std::move(done));
    }

    void drop(std::uint64_t group_id, std::uint64_t seq,
              dftracer::utils::comm::backend::Arrival* arrived) override {
        for (auto& instance : instances_)
            instance->drop(group_id, seq, arrived);
    }

    void release(std::uint64_t group_id) override {
        for (auto& instance : instances_) instance->release(group_id);
    }

   private:
    std::uint32_t driver_;
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::SharedPtr<
        dftracer::utils::comm::backend::Collectives>>
        instances_;
};

inline dftracer::utils::comm::Communicator start_per_rank(
    std::uint32_t ranks, dftracer::utils::coro::Executor& executor,
    std::uint32_t driver) {
    return start_processes(
        ranks, executor, process_per_rank(ranks),
        [ranks, driver](const auto& messaging) {
            return dftracer::utils::dstd::make_shared<PerRank>(messaging, ranks,
                                                               driver);
        });
}

inline bool
all_ok(const dftracer::utils::dstd::Vector<
       dftracer::utils::dstd::Optional<dftracer::utils::dstd::Error>>& errors) {
    bool ok = true;
    for (const auto& error : errors) {
        if (!error.has_value()) continue;
        MESSAGE(error->message());
        ok = false;
    }
    return ok;
}

}  // namespace dftu_comm_test

#endif  // DFTRACER_UTILS_TESTS_COMM_COLLECTIVE_RUNS_H
