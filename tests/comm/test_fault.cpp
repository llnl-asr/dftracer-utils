// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <comm/fault_runs.h>
#include <comm/support.h>
#include <dftracer/utils/comm/backend/fault_messaging.h>
#include <dftracer/utils/comm/backend/local.h>
#include <dftracer/utils/comm/channel.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/comm/op.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/yield.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

using namespace dftracer::utils;

namespace {

constexpr std::uint64_t LONG_NS = 20'000'000'000ull;

comm::CallOpts within(std::uint64_t ns) {
    comm::CallOpts opts;
    opts.deadline_ns = dftu_comm_test::deadline_after(ns);
    return opts;
}

comm::backend::MessageKey key_of(std::uint32_t source,
                                 std::uint32_t destination,
                                 std::uint64_t index = 1) {
    return comm::backend::MessageKey{
        7, source, destination,
        comm::backend::message_tag(comm::backend::TagPurpose::ChannelMessage,
                                   index)};
}

void send_one(comm::backend::Messaging& messaging,
              const comm::backend::MessageKey& key,
              dstd::Error* result = nullptr, bool* failed = nullptr) {
    messaging.send(key, dstd::Vector<std::byte>(8),
                   comm::backend::HOLD_EVERYTHING,
                   [result, failed](const dstd::Error* why) {
                       if (failed != nullptr) *failed = why != nullptr;
                       if (why != nullptr && result != nullptr) *result = *why;
                   });
}

struct Sent {
    std::uint32_t source = 0;
    std::uint64_t index = 0;
};

/// Sends three messages on each of four streams, round by round, through a
/// fault provider of @p config, and gives what it decided.
dstd::Vector<comm::backend::FaultEvent> run_script(
    comm::backend::FaultConfig config, dstd::Vector<Sent>* order = nullptr) {
    auto bootstrap = comm::backend::make_local_bootstrap(1);
    config.record_schedule = true;
    auto fault = comm::backend::make_fault_messaging(
        comm::backend::make_local_messaging(), std::move(config),
        bootstrap->timers());
    for (std::uint64_t round = 0; round < 3; ++round) {
        for (std::uint32_t source = 0; source < 4; ++source) {
            send_one(*fault, key_of(source, 9));
            if (order != nullptr) order->push_back(Sent{source, round});
        }
    }
    fault->flush();
    dstd::Vector<comm::backend::FaultEvent> events = fault->schedule();
    fault->abort(7, dstd::Error{dstd::Condition::Cancelled, "done"});
    return events;
}

bool same_events(dstd::Span<const comm::backend::FaultEvent> a,
                 dstd::Span<const comm::backend::FaultEvent> b) {
    if (a.size() != b.size()) return false;
    for (std::size_t at = 0; at < a.size(); ++at) {
        if (a[at].key.source != b[at].key.source ||
            a[at].index != b[at].index || a[at].action != b[at].action ||
            a[at].delay_ns != b[at].delay_ns)
            return false;
    }
    return true;
}

dstd::Vector<Sent> releases_of(
    dstd::Span<const comm::backend::FaultEvent> events) {
    dstd::Vector<Sent> out;
    for (const comm::backend::FaultEvent& event : events)
        if (event.action == comm::backend::FaultAction::Release)
            out.push_back(Sent{event.key.source, event.index});
    return out;
}

bool same_releases(dstd::Span<const Sent> a, dstd::Span<const Sent> b) {
    if (a.size() != b.size()) return false;
    for (std::size_t at = 0; at < a.size(); ++at)
        if (a[at].source != b[at].source || a[at].index != b[at].index)
            return false;
    return true;
}

}  // namespace

TEST_SUITE("comm fault injection") {
    TEST_CASE(
        "a killed rank fails every pending op of its group with one error "
        "that names it") {
        Runtime rt(4);
        comm::backend::FaultConfig config;
        comm::backend::FaultRule lose_requests;
        lose_requests.match.purpose = comm::backend::TagPurpose::BulkRequest;
        lose_requests.drop_per_million = 1'000'000;
        config.rules.push_back(lose_requests);
        dftu_comm_test::Faulty run =
            dftu_comm_test::start_faulty(3, *rt.executor(), std::move(config));
        comm::Communicator& communicator = run.communicator;

        dstd::Vector<std::byte> memory(64);
        comm::Group owner = communicator.world(0);
        auto ref =
            owner.expose(dstd::Span<std::byte>(memory.data(), memory.size()));
        REQUIRE(ref.has_value());

        dstd::Vector<dstd::Optional<dstd::Error>> barrier_error(3);
        dstd::Optional<dstd::Error> channel_error;
        dstd::Optional<dstd::Error> pull_error;
        auto errors = dftu_comm_test::run_ranks(
            rt, 3,
            [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                if (rank == 2) {
                    bool waiting = true;
                    while (waiting) {
                        co_await coro::yield();
                        waiting = communicator.pending().size() < 2 ||
                                  run.fault->counts().dropped < 1;
                    }
                    run.fault->kill(2);
                    co_return {};
                }
                auto barrier = group.barrier(within(LONG_NS));
                if (rank == 0) {
                    auto channel = group.channel<std::uint32_t>("quiet");
                    auto waiting = channel.recv_from(1, within(LONG_NS));
                    auto ended = co_await barrier;
                    if (!ended) barrier_error[rank] = ended.error();
                    auto got = co_await waiting;
                    if (!got) channel_error = got.error();
                    co_return {};
                }
                dstd::Vector<std::byte> into(64);
                auto pulling = group.pull(
                    *ref, dstd::Span<std::byte>(into.data(), into.size()), 0,
                    within(LONG_NS));
                auto ended = co_await barrier;
                if (!ended) barrier_error[rank] = ended.error();
                auto pulled = co_await pulling;
                if (!pulled) pull_error = pulled.error();
                co_return {};
            });
        CHECK_FALSE(errors[0].has_value());
        CHECK_FALSE(errors[1].has_value());
        CHECK_FALSE(errors[2].has_value());
        for (std::uint32_t rank = 0; rank < 2; ++rank) {
            INFO("rank " << rank);
            REQUIRE(barrier_error[rank].has_value());
            CHECK(dftu_comm_test::has_text(*barrier_error[rank],
                                           "rank 2 was killed"));
            CHECK(barrier_error[rank]->message() ==
                  barrier_error[0]->message());
        }
        REQUIRE(channel_error.has_value());
        CHECK(channel_error->message() == barrier_error[0]->message());
        REQUIRE(pull_error.has_value());
        CHECK(pull_error->message() == barrier_error[0]->message());
        CHECK(run.fault->counts().dropped >= 1);
    }

    TEST_CASE(
        "a dropped collective message ends by a deadline that names the "
        "missing rank") {
        Runtime rt(4);
        comm::backend::FaultConfig config;
        comm::backend::FaultRule lose_record;
        lose_record.match.source = 2;
        lose_record.match.destination = 0;
        lose_record.match.purpose = comm::backend::TagPurpose::CollectiveRecord;
        lose_record.drop_per_million = 1'000'000;
        lose_record.drop_limit = 1;
        config.rules.push_back(lose_record);
        dftu_comm_test::Faulty run =
            dftu_comm_test::start_faulty(3, *rt.executor(), std::move(config));
        comm::Communicator& communicator = run.communicator;
        auto errors = dftu_comm_test::run_ranks(
            rt, 3,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                const std::uint64_t value = rank;
                comm::CallOpts opts;
                if (rank == 0) opts = within(200'000'000ull);
                auto op = group.all_gather(
                    dstd::Span<const std::uint64_t>(&value, 1), opts);
                auto gathered = co_await op;
                if (!gathered) co_return dstd::Unexpected(gathered.error());
                co_return {};
            });
        for (std::uint32_t rank = 0; rank < 3; ++rank) {
            INFO("rank " << rank);
            REQUIRE(errors[rank].has_value());
            CHECK(errors[rank]->is(dstd::Condition::Timeout));
            CHECK(dftu_comm_test::has_text(*errors[rank], "missing ranks: 2"));
            CHECK(
                dftu_comm_test::has_text(*errors[rank], "arrived ranks: 0, 1"));
            CHECK(errors[rank]->message() == errors[0]->message());
        }
        // The limit is per stream: the record and the payload of the call are
        // two streams, and each loses its first message.
        CHECK(run.fault->counts().dropped == 2);
    }

    TEST_CASE(
        "a collective order mismatch is an error on every rank under "
        "delay") {
        Runtime rt(4);
        comm::backend::FaultConfig config;
        comm::backend::FaultRule slow;
        slow.delay_min_ns = 1'000'000;
        slow.delay_max_ns = 3'000'000;
        config.rules.push_back(slow);
        dftu_comm_test::Faulty run =
            dftu_comm_test::start_faulty(3, *rt.executor(), std::move(config));
        comm::Communicator& communicator = run.communicator;
        auto errors = dftu_comm_test::run_ranks(
            rt, 3,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                if (rank == 1) {
                    const std::uint64_t value = 1;
                    auto op = group.all_gather(
                        dstd::Span<const std::uint64_t>(&value, 1),
                        within(LONG_NS));
                    auto gathered = co_await op;
                    if (!gathered) co_return dstd::Unexpected(gathered.error());
                    co_return {};
                }
                auto op = group.barrier(within(LONG_NS));
                auto ended = co_await op;
                co_return ended;
            });
        for (std::uint32_t rank = 0; rank < 3; ++rank) {
            INFO("rank " << rank);
            REQUIRE(errors[rank].has_value());
            CHECK(dftu_comm_test::has_text(*errors[rank], "called"));
            CHECK_FALSE(errors[rank]->is(dstd::Condition::Timeout));
            CHECK(errors[rank]->message() == errors[0]->message());
        }
    }

    TEST_CASE(
        "delay and reorder keep the results right and each stream in "
        "order") {
        Runtime rt(4);
        comm::backend::FaultConfig config;
        config.seed = 11;
        config.reorder_flush_ns = 2'000'000;
        comm::backend::FaultRule reorder;
        reorder.match.purpose = comm::backend::TagPurpose::ChannelMessage;
        reorder.reorder_hold = 4;
        reorder.delay_min_ns = 0;
        reorder.delay_max_ns = 1'000'000;
        config.rules.push_back(reorder);
        comm::backend::FaultRule slow;
        slow.match.purpose = comm::backend::TagPurpose::CollectiveRecord;
        slow.delay_min_ns = 100'000;
        slow.delay_max_ns = 1'000'000;
        config.rules.push_back(slow);
        dftu_comm_test::Faulty run =
            dftu_comm_test::start_faulty(3, *rt.executor(), std::move(config));
        comm::Communicator& communicator = run.communicator;
        constexpr std::uint32_t COUNT = 40;
        auto errors = dftu_comm_test::run_ranks(
            rt, 3,
            [&communicator](
                std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                comm::ChannelOpts opts;
                opts.depth = 64;
                auto channel = group.channel<std::uint32_t>("fifo", opts);
                if (rank != 0) {
                    dstd::Vector<comm::Op<void>> sends;
                    for (std::uint32_t at = 0; at < COUNT; ++at)
                        sends.push_back(channel.send(0, rank * 1000 + at));
                    for (comm::Op<void>& send : sends) {
                        auto sent = co_await send;
                        if (!sent) co_return sent;
                    }
                } else {
                    for (std::uint32_t peer = 1; peer < 3; ++peer) {
                        for (std::uint32_t at = 0; at < COUNT; ++at) {
                            auto op = channel.recv_from(peer, within(LONG_NS));
                            auto message = co_await op;
                            if (!message)
                                co_return dstd::Unexpected(message.error());
                            CHECK(message->value() == peer * 1000 + at);
                        }
                    }
                }
                auto sum_op = group.all_reduce(
                    std::uint64_t{rank}, comm::ReduceOp::Sum, within(LONG_NS));
                auto sum = co_await sum_op;
                if (!sum) co_return dstd::Unexpected(sum.error());
                CHECK(*sum == 3);
                co_return {};
            });
        for (std::uint32_t rank = 0; rank < 3; ++rank) {
            INFO("rank " << rank);
            CHECK_FALSE(errors[rank].has_value());
        }
        const comm::backend::FaultCounts counts = run.fault->counts();
        CHECK(counts.held > 0);
        CHECK(counts.delayed > 0);
        CHECK(counts.dropped == 0);
    }

    TEST_CASE("the same seed gives the same schedule") {
        comm::backend::FaultConfig reorder;
        reorder.seed = 5;
        reorder.reorder_flush_ns = 60'000'000'000ull;
        comm::backend::FaultRule hold;
        hold.reorder_hold = 100;
        reorder.rules.push_back(hold);

        dstd::Vector<Sent> sent;
        const auto first = run_script(reorder, &sent);
        const auto again = run_script(reorder);
        CHECK(same_events(first, again));

        const dstd::Vector<Sent> released = releases_of(first);
        REQUIRE(released.size() == 12);
        dstd::Vector<std::uint64_t> next(4, 0);
        for (const Sent& one : released) {
            CHECK(one.index == next[one.source]);
            ++next[one.source];
        }
        CHECK_FALSE(same_releases(released, sent));

        comm::backend::FaultConfig other = reorder;
        other.seed = 6;
        const dstd::Vector<Sent> other_released =
            releases_of(run_script(other));
        REQUIRE(other_released.size() == 12);
        CHECK_FALSE(same_releases(released, other_released));

        comm::backend::FaultConfig delays;
        delays.seed = 9;
        comm::backend::FaultRule slow;
        slow.delay_min_ns = 1'000;
        slow.delay_max_ns = 9'000;
        delays.rules.push_back(slow);
        dstd::Vector<comm::backend::FaultEvent> decided;
        for (const comm::backend::FaultEvent& event : run_script(delays))
            if (event.action != comm::backend::FaultAction::Release)
                decided.push_back(event);
        dstd::Vector<comm::backend::FaultEvent> decided_again;
        for (const comm::backend::FaultEvent& event : run_script(delays))
            if (event.action != comm::backend::FaultAction::Release)
                decided_again.push_back(event);
        CHECK(same_events(decided, decided_again));
        REQUIRE(decided.size() == 12);
        for (const comm::backend::FaultEvent& event : decided) {
            CHECK(event.action == comm::backend::FaultAction::Delay);
            CHECK(event.delay_ns >= 1'000);
            CHECK(event.delay_ns <= 9'000);
        }
    }

    TEST_CASE(
        "a rank dies after its configured operations and its groups "
        "abort") {
        auto bootstrap = comm::backend::make_local_bootstrap(1);
        comm::backend::FaultConfig config;
        config.kills.push_back(comm::backend::FaultKill{1, 2});
        auto fault = comm::backend::make_fault_messaging(
            comm::backend::make_local_messaging(), std::move(config),
            bootstrap->timers());
        const std::uint32_t members[] = {0, 1, 2};
        fault->bind_group(7, dstd::Span<const std::uint32_t>(members, 3));
        const std::uint32_t others[] = {0, 2};
        fault->bind_group(8, dstd::Span<const std::uint32_t>(others, 2));

        std::mutex mutex;
        dstd::Vector<std::uint64_t> aborted;
        dstd::Optional<dstd::Error> abort_error;
        fault->on_peer_abort(
            [&](std::uint64_t group, std::uint64_t, const dstd::Error& why) {
                std::lock_guard<std::mutex> lock(mutex);
                aborted.push_back(group);
                abort_error = why;
            });

        dstd::Error first;
        bool failed = true;
        send_one(*fault, key_of(1, 0, 1), &first, &failed);
        CHECK_FALSE(failed);
        send_one(*fault, key_of(1, 0, 2), &first, &failed);
        CHECK_FALSE(failed);
        send_one(*fault, key_of(1, 0, 3), &first, &failed);
        CHECK(failed);
        CHECK(dftu_comm_test::has_text(first, "rank 1 was killed"));

        bool recv_failed = false;
        fault->recv(key_of(0, 1, 4),
                    [&](dstd::Span<const std::byte>, const dstd::Error* why) {
                        recv_failed = why != nullptr;
                    });
        CHECK(recv_failed);
        CHECK(fault->counts().killed_ops == 2);

        REQUIRE(dftu_comm_test::spin_until([&] {
            std::lock_guard<std::mutex> lock(mutex);
            return !aborted.empty();
        }));
        std::lock_guard<std::mutex> lock(mutex);
        REQUIRE(aborted.size() == 1);
        CHECK(aborted[0] == 7);
        REQUIRE(abort_error.has_value());
        CHECK(dftu_comm_test::has_text(*abort_error, "rank 1 was killed"));
        fault->abort(7, dstd::Error{dstd::Condition::Cancelled, "done"});
    }
}
