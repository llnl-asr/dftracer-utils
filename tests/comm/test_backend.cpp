// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <comm/support.h>
#include <dftracer/utils/comm/backend/linear_collectives.h>
#include <dftracer/utils/comm/backend/local.h>
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

    void send(const comm::backend::MessageKey& key,
              dstd::Vector<std::byte> bytes, SendDone done) override {
        traffic_.sends.fetch_add(1, std::memory_order_relaxed);
        moved_->send(key, std::move(bytes), std::move(done));
    }

    void recv(const comm::backend::MessageKey& key, RecvDone done) override {
        traffic_.recvs.fetch_add(1, std::memory_order_relaxed);
        moved_->recv(key, std::move(done));
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

    void send(const comm::backend::MessageKey& key,
              dstd::Vector<std::byte> bytes, SendDone done) override {
        if (key.source != 0 && !failed_.exchange(true)) {
            lost_ = dstd::Error{dstd::Condition::Io, "the message was lost"};
            done(&lost_);
            return;
        }
        moved_->send(key, std::move(bytes), std::move(done));
    }

    void recv(const comm::backend::MessageKey& key, RecvDone done) override {
        moved_->recv(key, std::move(done));
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
            messaging->send(stream, bytes_of(value), on_send);
        CHECK(sent == 0);

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
        CHECK(sent == 2);
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

        messaging->send(stream, bytes_of(11), [&order](const dstd::Error* why) {
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

        std::size_t failed_sends = 0;
        std::size_t failed_recvs = 0;
        bool other_ran = false;
        messaging->send(comm::backend::MessageKey{group, 0, 1, 1}, bytes_of(1),
                        [&failed_sends](const dstd::Error* why) {
                            REQUIRE(why != nullptr);
                            CHECK(why->is(dstd::Condition::Cancelled));
                            ++failed_sends;
                        });
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
        messaging->send(comm::backend::MessageKey{1, 0, 1, 1}, bytes_of(5),
                        [](const dstd::Error* why) { CHECK(why == nullptr); });
        messaging->recv(comm::backend::MessageKey{1, 0, 1, 1},
                        [](dstd::Span<const std::byte>,
                           const dstd::Error* why) { CHECK(why == nullptr); });
        CHECK(counts().parked == 0);
        messaging->release(1);
        CHECK(counts().streams == 0);

        // A parked message of an aborted group ends with the group.
        std::size_t failed_sends = 0;
        std::size_t failed_recvs = 0;
        messaging->send(comm::backend::MessageKey{2, 0, 1, 1}, bytes_of(5),
                        [&failed_sends](const dstd::Error* why) {
                            REQUIRE(why != nullptr);
                            ++failed_sends;
                        });
        messaging->recv(comm::backend::MessageKey{2, 1, 0, 2},
                        [&failed_recvs](dstd::Span<const std::byte>,
                                        const dstd::Error* why) {
                            REQUIRE(why != nullptr);
                            ++failed_recvs;
                        });
        CHECK(counts().parked == 2);
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
        messaging->send(comm::backend::MessageKey{5, 0, 1, 1}, bytes_of(5),
                        [&parked_sends](const dstd::Error* why) {
                            REQUIRE(why != nullptr);
                            CHECK(why->is(dstd::Condition::Cancelled));
                            ++parked_sends;
                        });
        messaging->recv(comm::backend::MessageKey{5, 1, 0, 2},
                        [&parked_recvs](dstd::Span<const std::byte>,
                                        const dstd::Error* why) {
                            REQUIRE(why != nullptr);
                            CHECK(why->is(dstd::Condition::Cancelled));
                            ++parked_recvs;
                        });
        CHECK(counts().parked == 2);
        messaging->release(5);
        CHECK(parked_sends == 1);
        CHECK(parked_recvs == 1);
        CHECK(counts().parked == 0);
        CHECK(counts().streams == 0);
        CHECK(counts().ended == 0);
    }
}
