// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <comm/support.h>
#include <dftracer/utils/comm/channel.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/comm/op.h>
#include <dftracer/utils/comm/stats.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <span_list.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <utility>
#include <vector>

using namespace dftracer::utils;

namespace {

constexpr std::uint64_t LONG_NS = 20'000'000'000ull;

comm::CallOpts within(std::uint64_t ns) {
    comm::CallOpts opts;
    opts.deadline_ns = dftu_comm_test::deadline_after(ns);
    return opts;
}

comm::Communicator start(std::uint32_t ranks, coro::Executor& executor) {
    auto started = comm::Communicator::start_local(ranks, executor);
    REQUIRE(started.has_value());
    return std::move(*started);
}

/// The counters of one kind of call of one group, zero when none was made.
comm::CallStats row(dstd::Span<const comm::CallStats> rows, std::uint64_t group,
                    dstd::StringView kind) {
    comm::CallStats found;
    for (const comm::CallStats& one : rows) {
        if (one.group != group || one.kind != kind) continue;
        found.started += one.started;
        found.ok += one.ok;
        found.failed += one.failed;
        found.cancelled += one.cancelled;
        found.bytes_sent += one.bytes_sent;
        found.bytes_received += one.bytes_received;
    }
    return found;
}

std::size_t count_events(dstd::StringView text) {
    std::size_t events = 0;
    dftu_utils_test::LineStream lines(text);
    dstd::String line;
    while (lines.next(line))
        if (!line.empty() && line[0] == '{') ++events;
    return events;
}

/// The lines of @p text that hold @p needle.
std::size_t count_lines_with(dstd::StringView text, dstd::StringView needle) {
    std::size_t found = 0;
    dftu_utils_test::LineStream lines(text);
    dstd::String line;
    while (lines.next(line))
        if (line.find(needle) != dstd::String::npos) ++found;
    return found;
}

/// The trace files of world rank @p rank in @p dir, by name.
dstd::Vector<dstd::String> trace_files(
    const dftu_utils_test::ScopedTestDir& dir, std::uint32_t rank) {
    const dstd::String prefix = "comm-" + dstd::to_string(rank) + "-" +
                                dstd::to_string(::getpid()) + "-";
    dstd::Vector<dstd::String> found;
    for (const auto& entry : dftu_utils_test::list_dir(dir.path().view())) {
        const dstd::StringView name = entry.path.view().filename();
        if (name.size() > prefix.size() &&
            name.starts_with(dstd::StringView(prefix)))
            found.push_back(dstd::String(entry.path.str()));
    }
    std::sort(found.begin(), found.end());
    return found;
}

}  // namespace

TEST_SUITE("comm observation") {
    TEST_CASE("a deadline names the group, the call, the age and who arrived") {
        Runtime rt(4);
        comm::Communicator communicator = start(3, *rt.executor());
        comm::Group group = communicator.world(0);
        const std::uint64_t value = 7;
        auto op = group.all_gather(dstd::Span<const std::uint64_t>(&value, 1),
                                   within(100'000'000ull));
        auto ended = op.wait();
        REQUIRE_FALSE(ended.has_value());
        const dstd::Error& why = ended.error();
        CHECK(why.is(dstd::Condition::Timeout));
        const dstd::String group_text =
            "group " + dstd::to_string(group.id()) + ":";
        CHECK(dftu_comm_test::has_text(why, group_text.c_str()));
        CHECK(dftu_comm_test::has_text(why, "all_gather at seq 0"));
        CHECK(dftu_comm_test::has_text(why, "deadline"));
        CHECK(dftu_comm_test::has_text(why, " ns; arrived ranks: 0;"));
        CHECK(dftu_comm_test::has_text(why, "missing ranks: 1, 2"));
    }

    TEST_CASE("pending shows a parked call and nothing once it ended") {
        Runtime rt(4);
        comm::Communicator communicator = start(3, *rt.executor());
        comm::Group first = communicator.world(0);
        comm::Group second = communicator.world(1);
        comm::Group third = communicator.world(2);
        CHECK(communicator.pending().empty());
        const std::uint64_t value = 1;
        const dstd::Span<const std::uint64_t> mine(&value, 1);
        auto parked = first.all_gather(mine, within(LONG_NS));
        const auto now = communicator.pending();
        REQUIRE(now.size() == 1);
        CHECK(now[0].group == first.id());
        CHECK(now[0].kind == "all_gather");
        CHECK(now[0].seq == 0);
        CHECK(now[0].rank == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        const auto later = communicator.pending();
        REQUIRE(later.size() == 1);
        CHECK(later[0].age_ns >= 20'000'000ull);
        auto from_second = second.all_gather(mine, within(LONG_NS));
        auto from_third = third.all_gather(mine, within(LONG_NS));
        CHECK(parked.wait().has_value());
        CHECK(from_second.wait().has_value());
        CHECK(from_third.wait().has_value());
        CHECK(communicator.pending().empty());
    }

    TEST_CASE("the counters of a scripted run are exact") {
        Runtime rt(4);
        comm::Communicator communicator = start(2, *rt.executor());
        comm::Group first = communicator.world(0);
        comm::Group second = communicator.world(1);
        const std::uint64_t world = first.id();
        const std::uint64_t value = 5;
        const dstd::Span<const std::uint64_t> mine(&value, 1);

        for (int round = 0; round < 3; ++round) {
            auto a = first.barrier();
            auto b = second.barrier();
            CHECK(a.wait().has_value());
            CHECK(b.wait().has_value());
        }
        for (int round = 0; round < 2; ++round) {
            auto a = first.all_gather(mine);
            auto b = second.all_gather(mine);
            CHECK(a.wait().has_value());
            CHECK(b.wait().has_value());
        }
        {
            auto a = first.all_reduce(value, comm::ReduceOp::Sum);
            auto b = second.all_reduce(value, comm::ReduceOp::Sum);
            CHECK(a.wait().has_value());
            CHECK(b.wait().has_value());
        }

        auto sender = first.channel<std::uint32_t>("counted");
        auto receiver = second.channel<std::uint32_t>("counted");
        for (std::uint32_t at = 0; at < 5; ++at)
            CHECK(sender.send(1, at).wait().has_value());
        for (std::uint32_t at = 0; at < 5; ++at)
            CHECK(receiver.recv_from(0).wait().has_value());

        dstd::Vector<std::byte> memory(100);
        auto ref =
            first.expose(dstd::Span<std::byte>(memory.data(), memory.size()));
        REQUIRE(ref.has_value());
        dstd::Vector<std::byte> into(100);
        auto pulled =
            second.pull(*ref, dstd::Span<std::byte>(into.data(), 100)).wait();
        REQUIRE(pulled.has_value());
        dstd::Vector<std::byte> from(40);
        auto pushed =
            second.push(*ref, dstd::Span<const std::byte>(from.data(), 40))
                .wait();
        REQUIRE(pushed.has_value());
        CHECK(first.unexpose(*ref).wait().has_value());

        const auto rows = communicator.stats();
        const comm::CallStats barrier = row(rows, world, "barrier");
        CHECK(barrier.started == 6);
        CHECK(barrier.ok == 6);
        CHECK(barrier.failed == 0);
        CHECK(barrier.bytes_sent == 0);
        const comm::CallStats gather = row(rows, world, "all_gather");
        CHECK(gather.started == 4);
        CHECK(gather.ok == 4);
        CHECK(gather.bytes_sent == 32);
        CHECK(gather.bytes_received == 64);
        const comm::CallStats reduce = row(rows, world, "all_reduce");
        CHECK(reduce.started == 2);
        CHECK(reduce.ok == 2);
        CHECK(reduce.bytes_sent == 16);
        CHECK(reduce.bytes_received == 16);
        const comm::CallStats sends = row(rows, world, "channel_send");
        CHECK(sends.started == 5);
        CHECK(sends.ok == 5);
        CHECK(sends.bytes_sent == 20);
        const comm::CallStats receives = row(rows, world, "channel_recv");
        CHECK(receives.started == 5);
        CHECK(receives.ok == 5);
        CHECK(receives.bytes_received == 20);
        const comm::CallStats pull = row(rows, world, "pull");
        CHECK(pull.started == 1);
        CHECK(pull.ok == 1);
        CHECK(pull.bytes_received == 100);
        const comm::CallStats push = row(rows, world, "push");
        CHECK(push.started == 1);
        CHECK(push.ok == 1);
        CHECK(push.bytes_sent == 40);

        // A cancel and a deadline end a call as cancelled and as failed, and a
        // group nothing names any more is counted in the row of group 0.
        std::uint64_t child_id = 0;
        {
            auto left = first.split(0, 0);
            auto right = second.split(0, 1);
            comm::Group child_first = *left.wait();
            comm::Group child_second = *right.wait();
            child_id = child_first.id();
            auto stopped = child_first.barrier();
            stopped.cancel();
            auto cancelled = stopped.wait();
            REQUIRE_FALSE(cancelled.has_value());
            CHECK(cancelled.error().is(dstd::Condition::Cancelled));
            const auto during = communicator.stats();
            const comm::CallStats child = row(during, child_id, "barrier");
            CHECK(child.started == 1);
            CHECK(child.cancelled == 1);
            CHECK(row(during, world, "split").ok == 2);
        }
        auto late = first.monitored_barrier(within(50'000'000ull));
        auto timed_out = late.wait();
        REQUIRE_FALSE(timed_out.has_value());
        const auto after = communicator.stats();
        const comm::CallStats missed = row(after, world, "monitored_barrier");
        CHECK(missed.started == 1);
        CHECK(missed.failed == 1);
        CHECK(row(after, child_id, "barrier").started == 0);
        const comm::CallStats retired = row(after, 0, "barrier");
        CHECK(retired.started == 1);
        CHECK(retired.cancelled == 1);
    }

    TEST_CASE(
        "the trace holds one event per call and a trace reader counts "
        "them") {
        dftu_utils_test::ScopedTestDir dir("comm_trace");
        ::setenv(common::constants::comm_settings::COMM_TRACE_ENV,
                 dir.str().c_str(), 1);
        {
            Runtime rt(4);
            comm::Communicator communicator = start(2, *rt.executor());
            comm::Group first = communicator.world(0);
            comm::Group second = communicator.world(1);
            const std::uint64_t value = 5;
            const dstd::Span<const std::uint64_t> mine(&value, 1);
            for (int round = 0; round < 3; ++round) {
                auto a = first.barrier();
                auto b = second.barrier();
                CHECK(a.wait().has_value());
                CHECK(b.wait().has_value());
            }
            auto a = first.all_gather(mine);
            auto b = second.all_gather(mine);
            CHECK(a.wait().has_value());
            CHECK(b.wait().has_value());
        }
        ::unsetenv(common::constants::comm_settings::COMM_TRACE_ENV);

        dstd::String files[2];
        for (std::uint32_t rank = 0; rank < 2; ++rank) {
            const auto named = trace_files(dir, rank);
            REQUIRE(named.size() == 1);
            files[rank] = named[0];
            const dstd::String text = dftu_utils_test::read_text(files[rank]);
            INFO("rank " << rank);
            REQUIRE_FALSE(text.empty());
            CHECK(count_events(text) == 4);
            CHECK(count_lines_with(text, R"("name":"barrier")") == 3);
            CHECK(count_lines_with(text, R"("name":"all_gather")") == 1);
            CHECK(count_lines_with(text, R"("cat":"comm")") == 4);
            CHECK(count_lines_with(text, R"("ph":1)") == 4);
            CHECK(count_lines_with(text, R"("status":"ok")") == 4);
            CHECK(count_lines_with(text, R"("seq":3,"bytes":24)") == 1);
        }

        const dstd::String binary = dftu_utils_test::find_binary_by_name(
            "DFTRACER_UTILS_EVENT_COUNT_PATH", "dftracer_event_count");
        REQUIRE_FALSE(binary.empty());
        const dstd::String counted = dftu_utils_test::run_process_capture(
            binary,
            dftu_utils_test::span_of<dstd::String>({"-f", files[0], files[1]}));
        dftu_utils_test::LineStream lines(counted);
        dstd::String line;
        dstd::String last;
        while (lines.next(line))
            if (!line.empty()) last = line;
        CHECK(last == "8");

        // Off by default: no variable, no file.
        {
            Runtime rt(2);
            comm::Communicator communicator = start(2, *rt.executor());
            comm::Group first = communicator.world(0);
            comm::Group second = communicator.world(1);
            auto a = first.barrier();
            auto b = second.barrier();
            CHECK(a.wait().has_value());
            CHECK(b.wait().has_value());
        }
        std::size_t entries = 0;
        for (const auto& entry : dftu_utils_test::walk_dir(dir.str())) {
            (void)entry;
            ++entries;
        }
        CHECK(entries == 2);
    }

    TEST_CASE("two communicators in one process write files of their own") {
        dftu_utils_test::ScopedTestDir dir("comm_trace_two");
        ::setenv(common::constants::comm_settings::COMM_TRACE_ENV,
                 dir.str().c_str(), 1);
        for (int which = 0; which < 2; ++which) {
            Runtime rt(4);
            comm::Communicator communicator = start(2, *rt.executor());
            comm::Group first = communicator.world(0);
            comm::Group second = communicator.world(1);
            for (int round = 0; round < 2 + which; ++round) {
                auto a = first.barrier();
                auto b = second.barrier();
                CHECK(a.wait().has_value());
                CHECK(b.wait().has_value());
            }
        }
        ::unsetenv(common::constants::comm_settings::COMM_TRACE_ENV);
        for (std::uint32_t rank = 0; rank < 2; ++rank) {
            INFO("rank " << rank);
            const auto named = trace_files(dir, rank);
            REQUIRE(named.size() == 2);
            CHECK(count_events(dftu_utils_test::read_text(named[0])) +
                      count_events(dftu_utils_test::read_text(named[1])) ==
                  5);
        }
    }

    TEST_CASE("a waiter finds its call counted, on every thread, every round") {
        constexpr std::uint32_t RANKS = 4;
        constexpr int ROUNDS = 3000;
        Runtime rt(4);
        comm::Communicator communicator = start(RANKS, *rt.executor());
        const std::uint64_t world = communicator.world(0).id();
        std::atomic<int> failures{0};
        std::vector<std::thread> threads;
        for (std::uint32_t rank = 0; rank < RANKS; ++rank) {
            threads.emplace_back([&, rank] {
                comm::Group group = communicator.world(rank);
                const std::uint64_t value = rank;
                const dstd::Span<const std::uint64_t> mine(&value, 1);
                for (int round = 1; round <= ROUNDS; ++round) {
                    if (!group.barrier().wait().has_value()) ++failures;
                    // The slot of this round ended, so every call of it and of
                    // the rounds before it was counted before this wait ended.
                    const comm::CallStats seen =
                        row(communicator.stats(), world, "barrier");
                    if (seen.ok < std::uint64_t{RANKS} * round) ++failures;
                    if (!group.all_gather(mine).wait().has_value()) ++failures;
                    const comm::CallStats gathered =
                        row(communicator.stats(), world, "all_gather");
                    if (gathered.ok < std::uint64_t{RANKS} * round) ++failures;
                }
            });
        }
        for (std::thread& thread : threads) thread.join();
        CHECK(failures.load() == 0);
        const auto rows = communicator.stats();
        CHECK(row(rows, world, "barrier").ok == RANKS * ROUNDS);
        CHECK(row(rows, world, "barrier").started == RANKS * ROUNDS);
        CHECK(row(rows, world, "all_gather").ok == RANKS * ROUNDS);
        CHECK(row(rows, world, "all_gather").bytes_sent == RANKS * ROUNDS * 8);
    }
}
