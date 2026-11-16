// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <comm/support.h>
#include <dftracer/utils/comm/backend/linear_collectives.h>
#include <dftracer/utils/comm/backend/local.h>
#include <dftracer/utils/comm/detail/channel_state.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/comm/op.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

using namespace dftracer::utils;

namespace {

/// Which collectives a communicator of the test runs on.
enum class Binding : std::uint8_t { Local, Linear };

constexpr Binding BINDINGS[] = {Binding::Local, Binding::Linear};

const char* binding_name(Binding binding) noexcept {
    return binding == Binding::Local
               ? "local collectives"
               : "linear collectives over local messaging";
}

/// Sends one message that its stream holds for a later receive, which is what a
/// test of the plain send of a collective wants.
comm::backend::OpId send_held(comm::backend::Messaging& messaging,
                              const comm::backend::MessageKey& key,
                              dstd::Vector<std::byte> bytes,
                              comm::backend::Messaging::SendDone done) {
    return messaging.send(key, std::move(bytes), comm::backend::HOLD_EVERYTHING,
                          std::move(done));
}

/// How much a run used its messaging, so a test proves the linear binding ran.
struct Traffic {
    std::atomic<std::size_t> sends{0};
    std::atomic<std::size_t> recvs{0};
};

/// Counts what it passes on, so a run proves the provider over it moved
/// messages.
class CountingMessaging : public comm::backend::Messaging {
   public:
    CountingMessaging(dstd::SharedPtr<comm::backend::Messaging> moved,
                      Traffic& traffic)
        : moved_(std::move(moved)), traffic_(traffic) {}

    comm::backend::OpId send(const comm::backend::MessageKey& key,
                             dstd::Vector<std::byte> bytes,
                             comm::backend::StreamOpts opts,
                             SendDone done) override {
        traffic_.sends.fetch_add(1, std::memory_order_relaxed);
        return moved_->send(key, std::move(bytes), opts, std::move(done));
    }

    comm::backend::OpId recv(const comm::backend::MessageKey& key,
                             RecvDone done) override {
        traffic_.recvs.fetch_add(1, std::memory_order_relaxed);
        return moved_->recv(key, std::move(done));
    }

    comm::backend::OpId recv_any_source(const comm::backend::MessageKey& key,
                                        std::uint32_t senders,
                                        RecvFromDone done) override {
        return moved_->recv_any_source(key, senders, std::move(done));
    }

    comm::backend::OpId recv_into(const comm::backend::MessageKey& key,
                                  std::uint32_t senders,
                                  dstd::Span<std::byte> buffer,
                                  RecvIntoDone done) override {
        return moved_->recv_into(key, senders, buffer, std::move(done));
    }

    bool cancel(const comm::backend::MessageKey& key, comm::backend::OpId op,
                dstd::Error why) override {
        return moved_->cancel(key, op, std::move(why));
    }

    void close(const comm::backend::MessageKey& key) override {
        moved_->close(key);
    }

    void abort(std::uint64_t group, dstd::Error why) override {
        moved_->abort(group, std::move(why));
    }

    void release(std::uint64_t group) override { moved_->release(group); }

   private:
    dstd::SharedPtr<comm::backend::Messaging> moved_;
    Traffic& traffic_;
};

comm::Communicator start(std::uint32_t ranks, coro::Executor& executor,
                         Binding binding, Traffic* traffic = nullptr) {
    auto started = comm::Communicator::start_local(ranks, executor);
    REQUIRE(started.has_value());
    comm::Communicator communicator = std::move(*started);
    dstd::SharedPtr<comm::backend::Messaging> messaging =
        comm::detail::Hooks::messaging(communicator);
    if (traffic != nullptr) {
        messaging = dstd::make_shared<CountingMessaging>(std::move(messaging),
                                                         *traffic);
    }
    if (binding == Binding::Linear) {
        comm::detail::Hooks::set_collectives(
            communicator,
            comm::backend::make_linear_collectives(std::move(messaging)));
    }
    return communicator;
}

/// What one rank got out of a split.
struct Child {
    std::uint32_t rank = 0;
    std::uint32_t size = 0;
    std::uint64_t id = 0;
};

/// A call: a barrier, a split that keeps every member, and one that keeps none.
void check_calls(Binding binding) {
    Runtime rt(4);
    Traffic traffic;
    comm::Communicator communicator =
        start(4, *rt.executor(), binding, &traffic);
    dstd::Vector<Child> children(4);
    auto errors = dftu_comm_test::run_ranks(
        rt, 4,
        [&communicator,
         &children](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
            comm::Group group = communicator.world(rank);
            auto barrier = co_await group.barrier();
            if (!barrier) co_return std::move(barrier).error();
            const std::uint32_t color = rank == 3 ? comm::Group::NO_COLOR : 2;
            auto child = co_await group.split(color, 3 - rank);
            if (!child) co_return std::move(child).error();
            children[rank] = Child{child->rank(), child->size(), child->id()};
            auto ended = co_await child->barrier();
            co_return ended;
        });
    for (std::uint32_t rank = 0; rank < 4; ++rank)
        CHECK_FALSE(errors[rank].has_value());

    CHECK(children[0].size == 3);
    CHECK(children[1].rank == 1);
    CHECK(children[0].rank == 2);
    CHECK(children[0].id == children[2].id);
    CHECK(children[0].id != communicator.world(0).id());
    CHECK(children[3].size == 0);
    CHECK(children[3].rank == 0);
    CHECK(children[3].id == 0);

    // The binding ran: a provider over the messaging moves records, the local
    // collectives do not touch it.
    if (binding == Binding::Linear) {
        CHECK(traffic.sends.load() > 0);
        CHECK(traffic.recvs.load() > 0);
    } else {
        CHECK(traffic.sends.load() == 0);
        CHECK(traffic.recvs.load() == 0);
    }
}

/// A deadline on one rank aborts the group for every rank, and a monitored
/// barrier names the members that did not arrive.
void check_timeout(Binding binding) {
    Runtime rt(4);
    comm::Communicator communicator = start(3, *rt.executor(), binding);
    std::atomic<bool> finished{false};
    auto errors = dftu_comm_test::run_ranks(
        rt, 3,
        [&communicator,
         &finished](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
            comm::Group group = communicator.world(rank);
            if (rank != 0) {
                co_await dftu_comm_test::wait_flag(finished);
                auto late = co_await group.barrier();
                co_return late;
            }
            comm::CallOpts opts;
            opts.deadline_ns = dftu_comm_test::deadline_after(40'000'000);
            auto late = co_await group.monitored_barrier(opts);
            finished.store(true, std::memory_order_release);
            co_return late;
        });
    REQUIRE(errors[0].has_value());
    CHECK(errors[0]->is(dstd::Condition::Timeout));
    CHECK(dftu_comm_test::has_text(*errors[0], "aborted by rank 0"));
    CHECK(dftu_comm_test::has_text(*errors[0], "missing ranks: 1, 2"));
    CHECK(dftu_comm_test::count_text(*errors[0], "group ") == 1);
    for (std::uint32_t rank = 1; rank < 3; ++rank) {
        REQUIRE(errors[rank].has_value());
        CHECK(errors[rank]->message() == errors[0]->message());
    }
}

/// A rank that speaks another protocol version fails the handshake of both.
void check_handshake(Binding binding) {
    Runtime rt(4);
    comm::Communicator communicator = start(2, *rt.executor(), binding);
    comm::detail::Hooks::set_protocol_version(communicator, 1, 99);
    auto errors = dftu_comm_test::run_ranks(
        rt, 2,
        [&communicator](
            std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
            comm::Group group = communicator.world(rank);
            auto ended = co_await group.barrier();
            co_return ended;
        });
    REQUIRE(errors[0].has_value());
    REQUIRE(errors[1].has_value());
    CHECK(errors[0]->is(dstd::Condition::Unsupported));
    CHECK(dftu_comm_test::has_text(*errors[0], "rank 0 speaks protocol 1"));
    CHECK(dftu_comm_test::has_text(*errors[0], "rank 1 speaks 99"));
    CHECK(dftu_comm_test::count_text(*errors[0], "group ") == 1);
    CHECK_FALSE(dftu_comm_test::has_text(*errors[0], "aborted by"));
    CHECK(errors[1]->message() == errors[0]->message());
}

/// A rank that calls another collective than its peer is an error, not a wait.
void check_mismatch(Binding binding) {
    Runtime rt(4);
    comm::Communicator communicator = start(2, *rt.executor(), binding);
    auto errors = dftu_comm_test::run_ranks(
        rt, 2,
        [&communicator](
            std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
            comm::Group group = communicator.world(rank);
            if (rank == 0) {
                auto ended = co_await group.barrier();
                co_return ended;
            }
            auto child = co_await group.split(1, 0);
            if (!child) co_return std::move(child).error();
            co_return {};
        });
    REQUIRE(errors[0].has_value());
    REQUIRE(errors[1].has_value());
    CHECK(errors[0]->is(dstd::Condition::InvalidArgument));
    CHECK(dftu_comm_test::has_text(*errors[0], "barrier"));
    CHECK(dftu_comm_test::has_text(*errors[0], "split"));
    CHECK(dftu_comm_test::count_text(*errors[0], "group ") == 1);
    CHECK(errors[1]->message() == errors[0]->message());
}

/// A cancel aborts the group for every rank, with one error.
void check_cancel(Binding binding) {
    Runtime rt(4);
    comm::Communicator communicator = start(2, *rt.executor(), binding);
    std::atomic<bool> finished{false};
    auto errors = dftu_comm_test::run_ranks(
        rt, 2,
        [&communicator,
         &finished](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
            comm::Group group = communicator.world(rank);
            if (rank != 0) {
                co_await dftu_comm_test::wait_flag(finished);
                auto late = co_await group.barrier();
                co_return late;
            }
            comm::Op<void> call = group.barrier();
            call.cancel();
            auto ended = co_await call;
            finished.store(true, std::memory_order_release);
            co_return ended;
        });
    REQUIRE(errors[0].has_value());
    CHECK(errors[0]->is(dstd::Condition::Cancelled));
    CHECK(dftu_comm_test::has_text(*errors[0], "canceled"));
    CHECK(dftu_comm_test::has_text(*errors[0], "aborted by rank 0"));
    REQUIRE(errors[1].has_value());
    CHECK(errors[1]->message() == errors[0]->message());
}

/// One call gives one outcome, and a group of one rank needs no peer.
void check_once(Binding binding) {
    Runtime rt(4);
    comm::Communicator communicator = start(2, *rt.executor(), binding);
    auto errors = dftu_comm_test::run_ranks(
        rt, 2,
        [&communicator](
            std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
            comm::Group group = communicator.world(rank);
            comm::Op<void> call = group.barrier();
            auto first = co_await call;
            if (!first) co_return std::move(first).error();
            auto second = call.wait();
            if (!second) co_return std::move(second).error();
            auto third = co_await group.barrier();
            co_return third;
        });
    CHECK_FALSE(errors[0].has_value());
    CHECK_FALSE(errors[1].has_value());

    comm::Communicator alone = start(1, *rt.executor(), binding);
    comm::Group group = alone.world(0);
    comm::Op<void> call = group.barrier();
    CHECK(call.test());
    CHECK(call.status() == comm::OpStatus::Ok);
}

/// A deadline on the root of a group names the members that did not arrive, and
/// every rank ends with that one error.
void check_monitored_root(Binding binding) {
    Runtime rt(4);
    comm::Communicator communicator = start(3, *rt.executor(), binding);
    std::atomic<bool> finished{false};
    auto errors = dftu_comm_test::run_ranks(
        rt, 3,
        [&communicator,
         &finished](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
            comm::Group group = communicator.world(rank);
            if (rank == 2) {
                co_await dftu_comm_test::wait_flag(finished);
                co_return dstd::Result<void>{};
            }
            comm::CallOpts opts;
            if (rank == 0)
                opts.deadline_ns = dftu_comm_test::deadline_after(40'000'000);
            auto late = co_await group.monitored_barrier(opts);
            if (rank == 0) finished.store(true, std::memory_order_release);
            co_return late;
        });
    REQUIRE(errors[0].has_value());
    CHECK(errors[0]->is(dstd::Condition::Timeout));
    CHECK(dftu_comm_test::has_text(*errors[0], "aborted by rank 0"));
    CHECK(dftu_comm_test::has_text(*errors[0], "missing ranks: 2"));
    REQUIRE(errors[1].has_value());
    CHECK(errors[1]->message() == errors[0]->message());
    CHECK_FALSE(errors[2].has_value());
}

/// A deadline on a member that is not the root names the missing members too,
/// while the root of the group did arrive.
void check_monitored_member(Binding binding) {
    Runtime rt(4);
    comm::Communicator communicator = start(3, *rt.executor(), binding);
    std::atomic<bool> finished{false};
    auto errors = dftu_comm_test::run_ranks(
        rt, 3,
        [&communicator,
         &finished](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
            comm::Group group = communicator.world(rank);
            if (rank == 2) {
                co_await dftu_comm_test::wait_flag(finished);
                co_return dstd::Result<void>{};
            }
            comm::CallOpts opts;
            if (rank == 1)
                opts.deadline_ns = dftu_comm_test::deadline_after(40'000'000);
            auto late = co_await group.monitored_barrier(opts);
            if (rank == 1) finished.store(true, std::memory_order_release);
            co_return late;
        });
    REQUIRE(errors[1].has_value());
    CHECK(errors[1]->is(dstd::Condition::Timeout));
    CHECK(dftu_comm_test::has_text(*errors[1], "aborted by rank 1"));
    CHECK(dftu_comm_test::has_text(*errors[1], "missing ranks: 2"));
    REQUIRE(errors[0].has_value());
    CHECK(errors[0]->message() == errors[1]->message());
}

/// A rank that only collects through another one reports the missing members it
/// cannot see instead of a guess.
void check_monitored_without_root(Binding binding) {
    Runtime rt(4);
    comm::Communicator communicator = start(3, *rt.executor(), binding);
    std::atomic<bool> finished{false};
    auto errors = dftu_comm_test::run_ranks(
        rt, 3,
        [&communicator,
         &finished](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
            comm::Group group = communicator.world(rank);
            if (rank == 0) {
                co_await dftu_comm_test::wait_flag(finished);
                co_return dstd::Result<void>{};
            }
            comm::CallOpts opts;
            if (rank == 1)
                opts.deadline_ns = dftu_comm_test::deadline_after(40'000'000);
            auto late = co_await group.monitored_barrier(opts);
            if (rank == 1) finished.store(true, std::memory_order_release);
            co_return late;
        });
    REQUIRE(errors[1].has_value());
    CHECK(errors[1]->is(dstd::Condition::Timeout));
    CHECK(dftu_comm_test::has_text(*errors[1], "aborted by rank 1"));
    if (binding == Binding::Local) {
        CHECK(dftu_comm_test::has_text(*errors[1], "missing ranks: 0"));
    } else {
        CHECK(dftu_comm_test::has_text(
            *errors[1], "missing ranks: not known to this rank"));
    }
}

/// The higher member of a disagreement arrives first, so the member that
/// arrives later is the lower one: the mismatch is found anyway.
void check_mismatch_after(Binding binding) {
    Runtime rt(4);
    comm::Communicator communicator = start(3, *rt.executor(), binding);
    std::atomic<bool> arrived{false};
    auto errors = dftu_comm_test::run_ranks(
        rt, 3,
        [&communicator,
         &arrived](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
            comm::Group group = communicator.world(rank);
            if (rank == 2) co_return dstd::Result<void>{};
            if (rank == 0) {
                co_await dftu_comm_test::wait_flag(arrived);
                auto ended = co_await group.barrier();
                co_return ended;
            }
            comm::Op<comm::Group> call = group.split(1, 0);
            arrived.store(true, std::memory_order_release);
            auto child = co_await call;
            if (!child) co_return std::move(child).error();
            co_return dstd::Result<void>{};
        });
    REQUIRE(errors[0].has_value());
    REQUIRE(errors[1].has_value());
    CHECK(errors[0]->is(dstd::Condition::InvalidArgument));
    CHECK(dftu_comm_test::has_text(*errors[0], "rank 0 called barrier"));
    CHECK(dftu_comm_test::has_text(*errors[0], "rank 1 called split"));
    CHECK(errors[1]->message() == errors[0]->message());
    CHECK_FALSE(errors[2].has_value());
}

/// Members that disagree on the call they make end with an error even when
/// another member never arrives, so a mismatch is never a wait.
void check_mismatch_missing(Binding binding) {
    Runtime rt(4);
    comm::Communicator communicator = start(3, *rt.executor(), binding);
    auto errors = dftu_comm_test::run_ranks(
        rt, 3,
        [&communicator](
            std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
            comm::Group group = communicator.world(rank);
            if (rank == 2) co_return dstd::Result<void>{};
            if (rank == 0) {
                auto ended = co_await group.barrier();
                co_return ended;
            }
            auto child = co_await group.split(1, 0);
            if (!child) co_return std::move(child).error();
            co_return dstd::Result<void>{};
        });
    REQUIRE(errors[0].has_value());
    REQUIRE(errors[1].has_value());
    CHECK(errors[0]->is(dstd::Condition::InvalidArgument));
    CHECK(dftu_comm_test::has_text(*errors[0], "rank 0 called barrier"));
    CHECK(dftu_comm_test::has_text(*errors[0], "rank 1 called split"));
    CHECK(errors[1]->message() == errors[0]->message());
    CHECK_FALSE(errors[2].has_value());
}

/// Fails the first record a member sends to the root of its group, as a lost
/// message does.
class FailingMessaging : public comm::backend::Messaging {
   public:
    explicit FailingMessaging(dstd::SharedPtr<comm::backend::Messaging> moved)
        : moved_(std::move(moved)) {}

    comm::backend::OpId send(const comm::backend::MessageKey& key,
                             dstd::Vector<std::byte> bytes,
                             comm::backend::StreamOpts opts,
                             SendDone done) override {
        if (key.source != 0 && !failed_.exchange(true)) {
            lost_ = dstd::Error{dstd::Condition::Io, "the message was lost"};
            done(&lost_);
            return comm::backend::NO_OP;
        }
        return moved_->send(key, std::move(bytes), opts, std::move(done));
    }

    comm::backend::OpId recv(const comm::backend::MessageKey& key,
                             RecvDone done) override {
        return moved_->recv(key, std::move(done));
    }

    comm::backend::OpId recv_any_source(const comm::backend::MessageKey& key,
                                        std::uint32_t senders,
                                        RecvFromDone done) override {
        return moved_->recv_any_source(key, senders, std::move(done));
    }

    comm::backend::OpId recv_into(const comm::backend::MessageKey& key,
                                  std::uint32_t senders,
                                  dstd::Span<std::byte> buffer,
                                  RecvIntoDone done) override {
        return moved_->recv_into(key, senders, buffer, std::move(done));
    }

    bool cancel(const comm::backend::MessageKey& key, comm::backend::OpId op,
                dstd::Error why) override {
        return moved_->cancel(key, op, std::move(why));
    }

    void close(const comm::backend::MessageKey& key) override {
        moved_->close(key);
    }

    void abort(std::uint64_t group, dstd::Error why) override {
        moved_->abort(group, std::move(why));
    }

    void release(std::uint64_t group) override { moved_->release(group); }

   private:
    dstd::SharedPtr<comm::backend::Messaging> moved_;
    std::atomic<bool> failed_{false};
    dstd::Error lost_;
};

/// A messaging error inside the provider ends the group with that error on
/// every rank.
void check_messaging_error() {
    Runtime rt(4);
    auto started = comm::Communicator::start_local(2, *rt.executor());
    REQUIRE(started.has_value());
    comm::Communicator communicator = std::move(*started);
    comm::detail::Hooks::set_collectives(
        communicator, comm::backend::make_linear_collectives(
                          dstd::make_shared<FailingMessaging>(
                              comm::detail::Hooks::messaging(communicator))));
    auto errors = dftu_comm_test::run_ranks(
        rt, 2,
        [&communicator](
            std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
            comm::Group group = communicator.world(rank);
            auto ended = co_await group.barrier();
            co_return ended;
        });
    REQUIRE(errors[0].has_value());
    REQUIRE(errors[1].has_value());
    CHECK(dftu_comm_test::has_text(*errors[0], "the message was lost"));
    CHECK(dftu_comm_test::has_text(*errors[0], "aborted by rank 1"));
    CHECK(errors[1]->message() == errors[0]->message());
}

/// A group whose calls all ended leaves nothing in the messaging it used.
void check_messaging_released(Binding binding) {
    Runtime rt(4);
    comm::Communicator communicator = start(4, *rt.executor(), binding);
    auto errors = dftu_comm_test::run_ranks(
        rt, 4,
        [&communicator](
            std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
            comm::Group group = communicator.world(rank);
            for (std::uint32_t round = 0; round < 3; ++round) {
                auto ended = co_await group.barrier();
                if (!ended) co_return std::move(ended).error();
            }
            co_return dstd::Result<void>{};
        });
    for (std::uint32_t rank = 0; rank < 4; ++rank)
        CHECK_FALSE(errors[rank].has_value());
    comm::detail::Hooks::release_groups(communicator);
    const comm::backend::LocalCounts counts = comm::backend::local_counts(
        *comm::detail::Hooks::messaging(communicator));
    CHECK(counts.streams == 0);
    CHECK(counts.parked == 0);
    CHECK(counts.ended == 0);
}

/// How a rank gives up on a group while two calls of two ranks are outstanding.
enum class GiveUp : std::uint8_t { Cancel, Deadline, Abort };

/// Two calls of two ranks are outstanding when one rank gives up on the group:
/// every rank that called ends with the same error and no rank waits.
///
/// The scenario runs on its own thread under a hang guard: a provider
/// completion that runs under the group mutex deadlocks instead of failing, and
/// a deadlock must fail this test, not stop the run.
void check_give_up_with_outstanding(Binding binding, GiveUp how) {
    std::atomic<bool> finished{false};
    std::atomic<int> made{0};
    std::atomic<bool> given_up{false};
    dstd::Vector<dstd::Optional<dstd::Error>> errors(3);
    std::thread runner([&] {
        Runtime rt(4);
        comm::Communicator communicator = start(3, *rt.executor(), binding);
        dstd::Vector<dstd::Optional<comm::Op<void>>> first(3);
        dstd::Vector<dstd::Optional<comm::Op<void>>> second(3);
        std::thread giver([&] {
            if (!dftu_comm_test::spin_until([&] { return made.load() == 2; }))
                return;
            if (how == GiveUp::Cancel) {
                first[0]->cancel();
            } else if (how == GiveUp::Abort) {
                comm::Group group = communicator.world(0);
                group.abort(
                    dstd::Error{dstd::Condition::Cancelled, "given up"});
            } else {
                // The deadline of rank 0 aborts the group from the timer.
                (void)dftu_comm_test::spin_until(
                    [&] { return first[0]->test(); });
            }
            given_up.store(true, std::memory_order_release);
        });
        errors = dftu_comm_test::run_ranks(
            rt, 3,
            [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                if (rank == 2) {
                    // Never calls: the group aborts with this rank missing.
                    co_await dftu_comm_test::wait_flag(given_up);
                    co_return dstd::Result<void>{};
                }
                comm::Group group = communicator.world(rank);
                comm::CallOpts opts;
                if (how == GiveUp::Deadline && rank == 0)
                    opts.deadline_ns =
                        dftu_comm_test::deadline_after(50'000'000);
                first[rank].emplace(group.barrier(opts));
                second[rank].emplace(group.barrier());
                made.fetch_add(1, std::memory_order_acq_rel);
                co_await dftu_comm_test::wait_flag(given_up);
                dstd::Result<void> one = co_await *first[rank];
                dstd::Result<void> two = co_await *second[rank];
                if (!one) {
                    two.discard();
                    co_return std::move(one).error();
                }
                co_return two;
            });
        giver.join();
        finished.store(true, std::memory_order_release);
    });
    const bool ended = dftu_comm_test::spin_until(
        [&] { return finished.load(std::memory_order_acquire); },
        std::chrono::seconds(20));
    if (!ended) {
        runner.detach();
        CHECK_MESSAGE(false,
                      "the group never ended: a deadlock on the group mutex");
        return;
    }
    runner.join();
    REQUIRE(errors[0].has_value());
    REQUIRE(errors[1].has_value());
    CHECK(errors[1]->message() == errors[0]->message());
    CHECK_FALSE(errors[2].has_value());
    CHECK(dftu_comm_test::has_text(*errors[0], "aborted by rank 0"));
    CHECK(dftu_comm_test::count_text(*errors[0], "group ") == 1);
    if (how == GiveUp::Deadline) {
        CHECK(errors[0]->is(dstd::Condition::Timeout));
    } else if (how == GiveUp::Cancel) {
        CHECK(errors[0]->is(dstd::Condition::Cancelled));
        CHECK(dftu_comm_test::has_text(*errors[0], "canceled"));
    } else {
        CHECK(errors[0]->is(dstd::Condition::Cancelled));
        CHECK(dftu_comm_test::has_text(*errors[0], "given up"));
    }
}

dstd::Vector<std::byte> bytes_of(std::uint32_t value) {
    dstd::Vector<std::byte> bytes(sizeof(value));
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

std::uint32_t value_of(dstd::Span<const std::byte> bytes) {
    REQUIRE(bytes.size() == sizeof(std::uint32_t));
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data(), sizeof(value));
    return value;
}

}  // namespace

TEST_SUITE("CommBackend") {
    TEST_CASE("Messaging keeps one stream per key in order") {
        auto messaging = comm::backend::make_local_messaging();
        const comm::backend::MessageKey stream{1, 0, 1, 7};
        const comm::backend::MessageKey other{1, 0, 1, 8};

        std::size_t sent = 0;
        auto on_send = [&sent](const dstd::Error* why) {
            CHECK(why == nullptr);
            ++sent;
        };
        for (std::uint32_t value = 1; value <= 3; ++value)
            send_held(*messaging, stream, bytes_of(value), on_send);
        // A stream that holds everything takes each message as it comes, so
        // every send ends once its message is in it.
        CHECK(sent == 3);

        dstd::Vector<std::uint32_t> took;
        bool other_ran = false;
        auto on_recv = [&took](dstd::Span<const std::byte> bytes,
                               const dstd::Error* why) {
            CHECK(why == nullptr);
            took.push_back(value_of(bytes));
        };
        for (std::uint32_t at = 0; at < 2; ++at)
            messaging->recv(stream, on_recv);
        messaging->recv(other,
                        [&other_ran](dstd::Span<const std::byte>,
                                     const dstd::Error*) { other_ran = true; });

        REQUIRE(took.size() == 2);
        CHECK(took[0] == 1);
        CHECK(took[1] == 2);
        CHECK(sent == 3);
        CHECK_FALSE(other_ran);

        messaging->recv(stream, on_recv);
        REQUIRE(took.size() == 3);
        CHECK(took[2] == 3);
        CHECK(sent == 3);
    }

    TEST_CASE("a recv posted before the send completes when it arrives") {
        auto messaging = comm::backend::make_local_messaging();
        const comm::backend::MessageKey stream{2, 1, 0, 3};

        dstd::Vector<const char*> order;
        std::uint32_t took = 0;
        messaging->recv(stream,
                        [&order, &took](dstd::Span<const std::byte> bytes,
                                        const dstd::Error* why) {
                            CHECK(why == nullptr);
                            took = value_of(bytes);
                            order.push_back("recv");
                        });
        CHECK(took == 0);
        CHECK(order.empty());

        send_held(*messaging, stream, bytes_of(11),
                  [&order](const dstd::Error* why) {
                      CHECK(why == nullptr);
                      order.push_back("send");
                  });
        CHECK(took == 11);
        REQUIRE(order.size() == 2);
        CHECK(std::strcmp(order[0], "recv") == 0);
        CHECK(std::strcmp(order[1], "send") == 0);
    }

    TEST_CASE("abort fails the parked sends and recvs of a group") {
        auto messaging = comm::backend::make_local_messaging();
        const std::uint64_t group = 3;
        const std::uint64_t other_group = 4;

        const comm::backend::StreamOpts one{1, comm::Overflow::Block};
        const comm::backend::MessageKey stream{group, 0, 1, 1};

        std::size_t held_sends = 0;
        std::size_t failed_sends = 0;
        std::size_t failed_recvs = 0;
        bool other_ran = false;
        auto send = [&](std::uint32_t value) {
            messaging->send(
                stream, bytes_of(value), one,
                [&held_sends, &failed_sends](const dstd::Error* why) {
                    if (why == nullptr) {
                        ++held_sends;
                        return;
                    }
                    CHECK(why->is(dstd::Condition::Cancelled));
                    ++failed_sends;
                });
        };
        // The first message fills the stream, so the second waits in it.
        send(1);
        send(2);
        messaging->recv(comm::backend::MessageKey{group, 1, 0, 2},
                        [&failed_recvs](dstd::Span<const std::byte>,
                                        const dstd::Error* why) {
                            REQUIRE(why != nullptr);
                            CHECK(why->is(dstd::Condition::Cancelled));
                            ++failed_recvs;
                        });
        messaging->recv(comm::backend::MessageKey{other_group, 1, 0, 2},
                        [&other_ran](dstd::Span<const std::byte>,
                                     const dstd::Error*) { other_ran = true; });

        messaging->abort(
            group, dstd::Error{dstd::Condition::Cancelled, "the group ended"});
        CHECK(held_sends == 1);
        CHECK(failed_sends == 1);
        CHECK(failed_recvs == 1);
        CHECK_FALSE(other_ran);
    }

    TEST_CASE("the API rules hold on both collectives") {
        for (const Binding binding : BINDINGS) {
            INFO(doctest::String(binding_name(binding)));
            check_calls(binding);
            check_timeout(binding);
            check_handshake(binding);
            check_mismatch(binding);
            check_cancel(binding);
            check_once(binding);
        }
    }

    TEST_CASE("a monitored deadline names the missing members on every rank") {
        for (const Binding binding : BINDINGS) {
            INFO(doctest::String(binding_name(binding)));
            check_monitored_root(binding);
            check_monitored_member(binding);
            check_monitored_without_root(binding);
        }
    }

    TEST_CASE("a mismatch ends the group when another member never arrives") {
        for (const Binding binding : BINDINGS) {
            INFO(doctest::String(binding_name(binding)));
            check_mismatch_missing(binding);
            check_mismatch_after(binding);
        }
    }

    TEST_CASE("a messaging error ends the group with that error") {
        check_messaging_error();
    }

    TEST_CASE("two outstanding calls end when a rank gives up") {
        for (const Binding binding : BINDINGS) {
            INFO(doctest::String(binding_name(binding)));
            check_give_up_with_outstanding(binding, GiveUp::Cancel);
            check_give_up_with_outstanding(binding, GiveUp::Deadline);
            check_give_up_with_outstanding(binding, GiveUp::Abort);
        }
    }

    TEST_CASE("a finished group holds nothing in the messaging") {
        for (const Binding binding : BINDINGS) {
            INFO(doctest::String(binding_name(binding)));
            check_messaging_released(binding);
        }
    }

    TEST_CASE("the local messaging ends a group and holds nothing of it") {
        auto messaging = comm::backend::make_local_messaging();
        auto counts = [&messaging] {
            return comm::backend::local_counts(*messaging);
        };

        // A message a receive took leaves no stream behind.
        send_held(*messaging, comm::backend::MessageKey{1, 0, 1, 1},
                  bytes_of(5),
                  [](const dstd::Error* why) { CHECK(why == nullptr); });
        messaging->recv(comm::backend::MessageKey{1, 0, 1, 1},
                        [](dstd::Span<const std::byte>,
                           const dstd::Error* why) { CHECK(why == nullptr); });
        CHECK(counts().parked == 0);
        messaging->release(1);
        CHECK(counts().streams == 0);

        // A parked message of an aborted group ends with the group.
        const comm::backend::StreamOpts one{1, comm::Overflow::Block};
        std::size_t failed_sends = 0;
        std::size_t failed_recvs = 0;
        auto send = [&](std::uint32_t value) {
            messaging->send(comm::backend::MessageKey{2, 0, 1, 1},
                            bytes_of(value), one,
                            [&failed_sends](const dstd::Error* why) {
                                if (why != nullptr) ++failed_sends;
                            });
        };
        // The first message fills the stream, so the second waits in it.
        send(1);
        send(2);
        messaging->recv(comm::backend::MessageKey{2, 1, 0, 2},
                        [&failed_recvs](dstd::Span<const std::byte>,
                                        const dstd::Error* why) {
                            REQUIRE(why != nullptr);
                            ++failed_recvs;
                        });
        // The message the stream holds, the one it holds back, and the receive.
        CHECK(counts().parked == 3);
        CHECK(counts().streams == 2);

        messaging->abort(
            2, dstd::Error{dstd::Condition::Cancelled, "the group ended"});
        CHECK(failed_sends == 1);
        CHECK(failed_recvs == 1);
        CHECK(counts().parked == 0);
        CHECK(counts().streams == 0);

        // The abort is sticky, so a later message of the group ends here too.
        std::size_t late = 0;
        messaging->recv(
            comm::backend::MessageKey{2, 1, 0, 3},
            [&late](dstd::Span<const std::byte>, const dstd::Error* why) {
                REQUIRE(why != nullptr);
                CHECK(why->is(dstd::Condition::Cancelled));
                ++late;
            });
        CHECK(late == 1);
        CHECK(counts().parked == 0);
        CHECK(counts().ended == 1);

        // A release forgets the end of a group whose last handle is gone.
        messaging->release(2);
        CHECK(counts().ended == 0);

        // A release of a group that never aborted fails what it still parks.
        std::size_t parked_sends = 0;
        std::size_t parked_recvs = 0;
        auto park_send = [&](std::uint32_t value) {
            messaging->send(comm::backend::MessageKey{5, 0, 1, 1},
                            bytes_of(value), one,
                            [&parked_sends](const dstd::Error* why) {
                                if (why == nullptr) return;
                                CHECK(why->is(dstd::Condition::Cancelled));
                                ++parked_sends;
                            });
        };
        park_send(1);
        park_send(2);
        messaging->recv(comm::backend::MessageKey{5, 1, 0, 2},
                        [&parked_recvs](dstd::Span<const std::byte>,
                                        const dstd::Error* why) {
                            REQUIRE(why != nullptr);
                            CHECK(why->is(dstd::Condition::Cancelled));
                            ++parked_recvs;
                        });
        CHECK(counts().parked == 3);
        messaging->release(5);
        CHECK(parked_sends == 1);
        CHECK(parked_recvs == 1);
        CHECK(counts().parked == 0);
        CHECK(counts().streams == 0);
        CHECK(counts().ended == 0);
    }
    TEST_CASE("a recv answered by a send leaves no stream behind") {
        auto messaging = comm::backend::make_local_messaging();
        std::size_t took = 0;
        for (std::uint64_t tag = 0; tag < 100; ++tag) {
            const comm::backend::MessageKey key{1, 0, 1, tag};
            messaging->recv(key, [&took](dstd::Span<const std::byte>,
                                         const dstd::Error* why) {
                CHECK(why == nullptr);
                ++took;
            });
            send_held(*messaging, key, bytes_of(1), [](const dstd::Error*) {});
        }
        CHECK(took == 100);
        CHECK(comm::backend::local_counts(*messaging).streams == 0);
    }

    TEST_CASE(
        "a send its stream stored has no handle and cannot be cancelled") {
        auto messaging = comm::backend::make_local_messaging();
        const comm::backend::MessageKey key{1, 0, 1, 1};
        std::size_t sent = 0;
        const comm::backend::OpId stored = send_held(
            *messaging, key, bytes_of(5), [&sent](const dstd::Error* why) {
                CHECK(why == nullptr);
                ++sent;
            });
        CHECK(sent == 1);
        CHECK(stored.value == 0);
        CHECK_FALSE(messaging->cancel(
            key, stored, dstd::Error{dstd::Condition::Cancelled, "late"}));
        std::uint32_t took = 0;
        messaging->recv(key, [&took](dstd::Span<const std::byte> bytes,
                                     const dstd::Error* why) {
            CHECK(why == nullptr);
            took = value_of(bytes);
        });
        CHECK(took == 5);
    }

    TEST_CASE("a send a full stream holds is cancelled and never delivered") {
        auto messaging = comm::backend::make_local_messaging();
        const comm::backend::MessageKey key{1, 0, 1, 1};
        const comm::backend::StreamOpts one{1, comm::Overflow::Block};
        messaging->send(key, bytes_of(1), one, [](const dstd::Error*) {});
        bool cancelled = false;
        const comm::backend::OpId held = messaging->send(
            key, bytes_of(2), one, [&cancelled](const dstd::Error* why) {
                REQUIRE(why != nullptr);
                CHECK(why->is(dstd::Condition::Cancelled));
                cancelled = true;
            });
        CHECK(held.value != 0);
        CHECK(messaging->cancel(
            key, held, dstd::Error{dstd::Condition::Cancelled, "stop"}));
        CHECK(cancelled);
        CHECK_FALSE(messaging->cancel(
            key, held, dstd::Error{dstd::Condition::Cancelled, "twice"}));
        dstd::Vector<std::uint32_t> took;
        for (int at = 0; at < 2; ++at)
            messaging->recv(key, [&took](dstd::Span<const std::byte> bytes,
                                         const dstd::Error* why) {
                if (why == nullptr) took.push_back(value_of(bytes));
            });
        REQUIRE(took.size() == 1);
        CHECK(took[0] == 1);
    }

    TEST_CASE("a close of a stream nothing was sent on answers its receive") {
        auto messaging = comm::backend::make_local_messaging();
        const comm::backend::MessageKey before{1, 0, 1, 1};
        const comm::backend::MessageKey after{1, 0, 1, 2};
        std::size_t closed = 0;
        auto on_recv = [&closed](dstd::Span<const std::byte>,
                                 const dstd::Error* why) {
            REQUIRE(why != nullptr);
            CHECK(why->is_code(comm::ChannelError::Closed));
            ++closed;
        };
        messaging->recv(before, on_recv);
        messaging->close(before);
        CHECK(closed == 1);
        messaging->close(after);
        messaging->recv(after, on_recv);
        CHECK(closed == 2);
    }

    TEST_CASE("a receive of any source ends Closed once every sender closed") {
        auto messaging = comm::backend::make_local_messaging();
        const comm::backend::MessageKey any{1, comm::backend::ANY_SOURCE, 0, 1};
        const comm::backend::MessageKey from_one{1, 1, 0, 1};
        const comm::backend::MessageKey from_two{1, 2, 0, 1};
        std::size_t closed = 0;
        std::size_t took = 0;
        auto on_recv = [&closed, &took](std::uint32_t,
                                        dstd::Span<const std::byte>,
                                        const dstd::Error* why) {
            if (why == nullptr) {
                ++took;
                return;
            }
            CHECK(why->is_code(comm::ChannelError::Closed));
            ++closed;
        };
        send_held(*messaging, from_one, bytes_of(1), [](const dstd::Error*) {});
        messaging->close(from_one);
        messaging->recv_any_source(any, 2, on_recv);
        CHECK(took == 1);
        // One sender closed and the other did not, so this receive waits.
        messaging->recv_any_source(any, 2, on_recv);
        CHECK(closed == 0);
        send_held(*messaging, from_two, bytes_of(2), [](const dstd::Error*) {});
        CHECK(took == 2);
        messaging->recv_any_source(any, 2, on_recv);
        CHECK(closed == 0);
        messaging->close(from_two);
        CHECK(closed == 1);
        messaging->recv_any_source(any, 2, on_recv);
        CHECK(closed == 2);
    }

    TEST_CASE("a receive of any source serves the senders in turn") {
        auto messaging = comm::backend::make_local_messaging();
        const comm::backend::MessageKey any{1, comm::backend::ANY_SOURCE, 0, 1};
        for (std::uint32_t source = 1; source <= 3; ++source)
            for (std::uint32_t at = 0; at < 3; ++at)
                send_held(*messaging,
                          comm::backend::MessageKey{1, source, 0, 1},
                          bytes_of(source), [](const dstd::Error*) {});
        dstd::Vector<std::uint32_t> order;
        for (int at = 0; at < 9; ++at)
            messaging->recv_any_source(
                any, 3,
                [&order](std::uint32_t source, dstd::Span<const std::byte>,
                         const dstd::Error* why) {
                    CHECK(why == nullptr);
                    order.push_back(source);
                });
        REQUIRE(order.size() == 9);
        for (std::size_t at = 0; at < order.size(); ++at)
            CHECK(order[at] == 1 + at % 3);
    }
    TEST_CASE("a declaration between a read and a watch is not lost") {
        auto messaging = comm::backend::make_local_messaging();
        auto hub =
            dstd::make_shared<comm::detail::ChannelHub>(messaging, 1, 1, 2);
        const std::uint64_t first = 111;
        const std::uint64_t second = 222;
        hub->note_open(first, 4);
        hub->note_open(second, 4);
        auto call_a = dstd::make_shared<comm::OpState<void>>();
        auto call_b = dstd::make_shared<comm::OpState<void>>();
        dstd::WeakPtr<comm::PendingOp> weak_a = call_a;
        dstd::WeakPtr<comm::PendingOp> weak_b = call_b;
        std::size_t woke_a = 0;
        std::size_t woke_b = 0;
        REQUIRE(hub->watch(
            0, hub->make_ticket(), weak_a,
            [&woke_a](const dstd::Error*) { ++woke_a; }, 0));

        comm::detail::ChannelOpen wrong;
        std::uint64_t seen = 0;
        CHECK(hub->open(0, first, wrong, seen) ==
              comm::detail::ChannelHub::Open::Waiting);

        // The peer declares the channel after the read and before the watch.
        comm::detail::ChannelOpen declared{first, 4, 0};
        dstd::Vector<std::byte> bytes(sizeof(declared));
        std::memcpy(bytes.data(), &declared, sizeof(declared));
        send_held(*messaging,
                  comm::backend::MessageKey{
                      1, 0, 1,
                      comm::backend::message_tag(
                          comm::backend::TagPurpose::ChannelOpen, 0)},
                  std::move(bytes), [](const dstd::Error*) {});
        CHECK(woke_a == 1);

        const bool is_waiting = hub->watch(
            0, hub->make_ticket(), weak_b,
            [&woke_b](const dstd::Error*) { ++woke_b; }, seen);
        CHECK_FALSE(is_waiting);
        CHECK(woke_b == 0);
        CHECK(hub->open(0, first, wrong, seen) ==
              comm::detail::ChannelHub::Open::Matched);
    }
}
