// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <comm/collective_runs.h>
#include <comm/support.h>
#include <dftracer/utils/comm/backend/arrival_fold.h>
#include <dftracer/utils/comm/backend/linear_collectives.h>
#include <dftracer/utils/comm/backend/local.h>
#include <dftracer/utils/comm/collective.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/comm/op.h>
#include <dftracer/utils/dstd/clock.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>

using namespace dftracer::utils;

namespace {

using dftu_comm_test::all_ok;
using dftu_comm_test::Binding;
using dftu_comm_test::binding_name;
using dftu_comm_test::BINDINGS;
using dftu_comm_test::bytes_of;
using dftu_comm_test::for_each_run;
using dftu_comm_test::has_text;
using dftu_comm_test::PerRank;
using dftu_comm_test::RANK_COUNTS;
using dftu_comm_test::run_ranks;
using dftu_comm_test::start;
using dftu_comm_test::start_per_rank;
using dftu_comm_test::text_of;

/// The text rank @p rank brings: its size differs by rank, and rank 1 of a
/// run brings none.
std::string word_of(std::uint32_t rank) {
    return rank == 1 ? std::string() : std::string(rank + 2, char('a' + rank));
}

struct Best {
    std::int32_t score;
    std::int32_t who;
};

}  // namespace

TEST_SUITE("CommCollectives") {
    TEST_CASE("a broadcast gives every rank the value of the root") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            const std::uint32_t root = ranks - 1;
            dstd::Vector<std::int64_t> scalar(ranks);
            dstd::Vector<dstd::Vector<std::int32_t>> bulk(ranks);
            dstd::Vector<std::string> text(ranks);
            auto errors = run_ranks(
                rt, ranks,
                [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                    comm::Group group = communicator.world(rank);
                    const std::int64_t mine = rank == root ? 1234 : -1;
                    auto one_op = group.broadcast(root, mine);
                    auto one = co_await one_op;
                    if (!one) co_return std::move(one).error();
                    scalar[rank] = *one;
                    dstd::Vector<std::int32_t> values = {7, 8, 9};
                    if (rank != root) values = {0, 0, 0};
                    auto many_op = group.broadcast(
                        root, dstd::Span<const std::int32_t>(values));
                    auto many = co_await many_op;
                    if (!many) co_return std::move(many).error();
                    bulk[rank] = *many;
                    const std::string word = rank == root ? "payload" : "";
                    auto bytes_op = group.broadcast(root, bytes_of(word));
                    auto bytes = co_await bytes_op;
                    if (!bytes) co_return std::move(bytes).error();
                    text[rank] = text_of(bytes->view);
                    co_return {};
                });
            CHECK(all_ok(errors));
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                CHECK(scalar[rank] == 1234);
                CHECK(bulk[rank] == dstd::Vector<std::int32_t>{7, 8, 9});
                CHECK(text[rank] == "payload");
            }
        });
    }

    TEST_CASE("a broadcast of nothing ends with nothing") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            std::atomic<int> empty{0};
            auto errors = run_ranks(
                rt, ranks,
                [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                    comm::Group group = communicator.world(rank);
                    auto bytes_op = group.broadcast(
                        ranks - 1, dstd::Span<const std::byte>());
                    auto bytes = co_await bytes_op;
                    if (!bytes) co_return std::move(bytes).error();
                    if (bytes->size() == 0) empty.fetch_add(1);
                    co_return {};
                });
            CHECK(all_ok(errors));
            CHECK(empty.load() == static_cast<int>(ranks));
        });
    }

    TEST_CASE("a gather brings every rank's values to the root only") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            const std::uint32_t root = ranks - 1;
            dstd::Vector<comm::Gathered> typed(ranks);
            dstd::Vector<comm::Gathered> variable(ranks);
            auto errors = run_ranks(
                rt, ranks,
                [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                    comm::Group group = communicator.world(rank);
                    const dstd::Vector<std::uint32_t> values = {rank * 10,
                                                                rank * 10 + 1};
                    auto one_op = group.gather(
                        root, dstd::Span<const std::uint32_t>(values));
                    auto one = co_await one_op;
                    if (!one) co_return std::move(one).error();
                    typed[rank] = *one;
                    const std::string word = word_of(rank);
                    auto two_op = group.gather(root, bytes_of(word));
                    auto two = co_await two_op;
                    if (!two) co_return std::move(two).error();
                    variable[rank] = *two;
                    co_return {};
                });
            CHECK(all_ok(errors));
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                if (rank != root) {
                    CHECK(typed[rank].size() == 0);
                    CHECK(variable[rank].size() == 0);
                    continue;
                }
                REQUIRE(typed[rank].size() == ranks);
                REQUIRE(variable[rank].size() == ranks);
                for (std::uint32_t from = 0; from < ranks; ++from) {
                    const auto values = typed[rank].as<std::uint32_t>(from);
                    REQUIRE(values.size() == 2);
                    CHECK(values[0] == from * 10);
                    CHECK(values[1] == from * 10 + 1);
                    CHECK(text_of(variable[rank][from]) == word_of(from));
                }
            }
        });
    }

    TEST_CASE(
        "an all_gather gives every rank one buffer with a view per rank") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            dstd::Vector<comm::Gathered> got(ranks);
            auto errors = run_ranks(
                rt, ranks,
                [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                    comm::Group group = communicator.world(rank);
                    const std::string word = word_of(rank);
                    auto all_op = group.all_gather(bytes_of(word));
                    auto all = co_await all_op;
                    if (!all) co_return std::move(all).error();
                    got[rank] = *all;
                    co_return {};
                });
            CHECK(all_ok(errors));
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                REQUIRE(got[rank].size() == ranks);
                const std::byte* at = got[rank].data().data();
                for (std::uint32_t from = 0; from < ranks; ++from) {
                    CHECK(text_of(got[rank][from]) == word_of(from));
                    // One buffer: the views lie one after the other.
                    if (!got[rank][from].empty()) {
                        CHECK(got[rank][from].data() == at);
                    }
                    at += got[rank][from].size();
                }
                CHECK(got[rank].data().data() == got[0].data().data());
            }
        });
    }

    TEST_CASE("typed all_gather of equal counts reads as values") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            dstd::Vector<comm::Gathered> got(ranks);
            auto errors = run_ranks(
                rt, ranks,
                [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                    comm::Group group = communicator.world(rank);
                    const dstd::Vector<double> values = {rank + 0.5,
                                                         rank + 0.25};
                    auto all_op =
                        group.all_gather(dstd::Span<const double>(values));
                    auto all = co_await all_op;
                    if (!all) co_return std::move(all).error();
                    got[rank] = *all;
                    co_return {};
                });
            CHECK(all_ok(errors));
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                REQUIRE(got[rank].size() == ranks);
                for (std::uint32_t from = 0; from < ranks; ++from) {
                    const auto values = got[rank].as<double>(from);
                    REQUIRE(values.size() == 2);
                    CHECK(values[0] == from + 0.5);
                    CHECK(values[1] == from + 0.25);
                }
            }
        });
    }

    TEST_CASE("empty payloads gather as empty views") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            dstd::Vector<comm::Gathered> all(ranks);
            dstd::Vector<comm::Gathered> root_only(ranks);
            auto errors = run_ranks(
                rt, ranks,
                [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                    comm::Group group = communicator.world(rank);
                    auto one_op =
                        group.all_gather(dstd::Span<const std::byte>());
                    auto one = co_await one_op;
                    if (!one) co_return std::move(one).error();
                    all[rank] = *one;
                    auto two_op =
                        group.gather(0, dstd::Span<const std::int32_t>());
                    auto two = co_await two_op;
                    if (!two) co_return std::move(two).error();
                    root_only[rank] = *two;
                    co_return {};
                });
            CHECK(all_ok(errors));
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                CHECK(all[rank].size() == ranks);
                CHECK(all[rank].data().empty());
                for (std::uint32_t from = 0; from < ranks; ++from)
                    CHECK(all[rank][from].empty());
            }
            CHECK(root_only[0].size() == ranks);
        });
    }

    TEST_CASE("a scatter hands rank r piece r of the root") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            const std::uint32_t root = ranks - 1;
            dstd::Vector<dstd::Vector<std::int32_t>> typed(ranks);
            dstd::Vector<std::string> variable(ranks);
            auto errors = run_ranks(
                rt, ranks,
                [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                    comm::Group group = communicator.world(rank);
                    dstd::Vector<std::int32_t> values;
                    dstd::Vector<std::string> words;
                    dstd::Vector<dstd::Span<const std::byte>> pieces;
                    if (rank == root) {
                        for (std::uint32_t at = 0; at < ranks * 2; ++at)
                            values.push_back(static_cast<std::int32_t>(at));
                        for (std::uint32_t at = 0; at < ranks; ++at)
                            words.push_back(word_of(at));
                        for (const std::string& word : words)
                            pieces.push_back(bytes_of(word));
                    }
                    auto one_op = group.scatter(
                        root, dstd::Span<const std::int32_t>(values));
                    auto one = co_await one_op;
                    if (!one) co_return std::move(one).error();
                    typed[rank] = *one;
                    auto two_op = group.scatter(
                        root,
                        dstd::Span<const dstd::Span<const std::byte>>(pieces));
                    auto two = co_await two_op;
                    if (!two) co_return std::move(two).error();
                    variable[rank] = text_of(two->view);
                    co_return {};
                });
            CHECK(all_ok(errors));
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                CHECK(typed[rank] ==
                      dstd::Vector<std::int32_t>{
                          static_cast<std::int32_t>(rank * 2),
                          static_cast<std::int32_t>(rank * 2 + 1)});
                CHECK(variable[rank] == word_of(rank));
            }
        });
    }

    TEST_CASE("a scatter that the ranks do not divide fails on every rank") {
        for (const Binding binding : BINDINGS) {
            CAPTURE(binding_name(binding));
            Runtime rt(4);
            comm::Communicator communicator = start(3, *rt.executor(), binding);
            auto errors = run_ranks(
                rt, 3,
                [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                    comm::Group group = communicator.world(rank);
                    const dstd::Vector<std::int32_t> values = {1, 2, 3, 4};
                    auto got_op = group.scatter(
                        0, rank == 0 ? dstd::Span<const std::int32_t>(values)
                                     : dstd::Span<const std::int32_t>());
                    auto got = co_await got_op;
                    if (!got) co_return std::move(got).error();
                    co_return {};
                });
            for (const auto& error : errors) {
                REQUIRE(error.has_value());
                CHECK(has_text(*error, "do not split"));
            }
        }
    }

    TEST_CASE("reduce and all_reduce merge with Sum, Min and Max") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            const std::uint32_t root = ranks - 1;
            dstd::Vector<std::int64_t> sum(ranks);
            dstd::Vector<std::int64_t> low(ranks);
            dstd::Vector<std::int64_t> high(ranks);
            dstd::Vector<dstd::Vector<double>> rooted(ranks);
            auto errors = run_ranks(
                rt, ranks,
                [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                    comm::Group group = communicator.world(rank);
                    const std::int64_t mine = rank + 1;
                    auto a_op = group.all_reduce(mine, comm::ReduceOp::Sum);
                    auto a = co_await a_op;
                    if (!a) co_return std::move(a).error();
                    sum[rank] = *a;
                    auto b_op = group.all_reduce(mine, comm::ReduceOp::Min);
                    auto b = co_await b_op;
                    if (!b) co_return std::move(b).error();
                    low[rank] = *b;
                    auto c_op = group.all_reduce(mine, comm::ReduceOp::Max);
                    auto c = co_await c_op;
                    if (!c) co_return std::move(c).error();
                    high[rank] = *c;
                    const dstd::Vector<double> values = {rank * 1.5, 2.0};
                    auto d_op =
                        group.reduce(root, dstd::Span<const double>(values),
                                     comm::ReduceOp::Sum);
                    auto d = co_await d_op;
                    if (!d) co_return std::move(d).error();
                    rooted[rank] = *d;
                    co_return {};
                });
            CHECK(all_ok(errors));
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                CHECK(sum[rank] == std::int64_t(ranks) * (ranks + 1) / 2);
                CHECK(low[rank] == 1);
                CHECK(high[rank] == ranks);
                if (rank != root) {
                    CHECK(rooted[rank].empty());
                    continue;
                }
                REQUIRE(rooted[rank].size() == 2);
                CHECK(rooted[rank][0] == 1.5 * ranks * (ranks - 1) / 2);
                CHECK(rooted[rank][1] == 2.0 * ranks);
            }
        });
    }

    TEST_CASE("a reduction runs the binary function of the user") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            dstd::Vector<std::int64_t> product(ranks);
            dstd::Vector<Best> best(ranks);
            auto errors = run_ranks(
                rt, ranks,
                [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                    comm::Group group = communicator.world(rank);
                    const std::int64_t mine = rank + 1;
                    auto a_op = group.all_reduce(
                        mine, [](const std::int64_t& x, const std::int64_t& y) {
                            return x * y;
                        });
                    auto a = co_await a_op;
                    if (!a) co_return std::move(a).error();
                    product[rank] = *a;
                    const Best entry{static_cast<std::int32_t>((rank * 7) % 5),
                                     static_cast<std::int32_t>(rank)};
                    auto b_op = group.all_reduce(
                        entry, [](const Best& x, const Best& y) {
                            return y.score > x.score ? y : x;
                        });
                    auto b = co_await b_op;
                    if (!b) co_return std::move(b).error();
                    best[rank] = *b;
                    co_return {};
                });
            CHECK(all_ok(errors));
            std::int64_t factorial = 1;
            for (std::uint32_t at = 1; at <= ranks; ++at) factorial *= at;
            Best expected{-1, -1};
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                const Best entry{static_cast<std::int32_t>((rank * 7) % 5),
                                 static_cast<std::int32_t>(rank)};
                if (entry.score > expected.score) expected = entry;
            }
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                CHECK(product[rank] == factorial);
                CHECK(best[rank].score == expected.score);
                CHECK(best[rank].who == expected.who);
            }
        });
    }

    TEST_CASE("a float sum is the same on every run and every rank") {
        // The values lose a unit at the other order, so only the fold in rank
        // order gives the result below.
        constexpr float VALUES[] = {1.0f, 1e8f, -1e8f, 1.0f};
        for (const Binding binding : BINDINGS) {
            CAPTURE(binding_name(binding));
            for (int run = 0; run < 20; ++run) {
                Runtime rt(4);
                comm::Communicator communicator =
                    start(3, *rt.executor(), binding);
                dstd::Vector<float> got(3);
                auto errors =
                    run_ranks(rt, 3,
                              [&](std::uint32_t rank)
                                  -> coro::CoroTask<dstd::Result<void>> {
                                  comm::Group group = communicator.world(rank);
                                  auto sum_op = group.all_reduce(
                                      VALUES[rank], comm::ReduceOp::Sum);
                                  auto sum = co_await sum_op;
                                  if (!sum) co_return std::move(sum).error();
                                  got[rank] = *sum;
                                  co_return {};
                              });
                CHECK(all_ok(errors));
                float expected = VALUES[0];
                expected += VALUES[1];
                expected += VALUES[2];
                float reversed = VALUES[2];
                reversed += VALUES[1];
                reversed += VALUES[0];
                REQUIRE(expected != reversed);
                for (std::uint32_t rank = 0; rank < 3; ++rank)
                    CHECK(std::memcmp(&got[rank], &expected, sizeof(float)) ==
                          0);
            }
        }
    }

    TEST_CASE("the arrival order option folds in the provider") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            dstd::Vector<std::int64_t> sum(ranks);
            dstd::Vector<dstd::Vector<std::int64_t>> rooted(ranks);
            auto errors = run_ranks(
                rt, ranks,
                [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                    comm::Group group = communicator.world(rank);
                    comm::CallOpts opts;
                    opts.reduce_order = comm::ReduceOrder::Any;
                    const std::int64_t mine = rank + 1;
                    auto a_op =
                        group.all_reduce(mine, comm::ReduceOp::Sum, opts);
                    auto a = co_await a_op;
                    if (!a) co_return std::move(a).error();
                    sum[rank] = *a;
                    const dstd::Vector<std::int64_t> values = {mine, 10};
                    auto b_op = group.reduce(
                        ranks - 1, dstd::Span<const std::int64_t>(values),
                        comm::ReduceOp::Max, opts);
                    auto b = co_await b_op;
                    if (!b) co_return std::move(b).error();
                    rooted[rank] = *b;
                    co_return {};
                });
            CHECK(all_ok(errors));
            for (std::uint32_t rank = 0; rank < ranks; ++rank)
                CHECK(sum[rank] == std::int64_t(ranks) * (ranks + 1) / 2);
            REQUIRE(rooted[ranks - 1].size() == 2);
            CHECK(rooted[ranks - 1][0] == ranks);
            CHECK(rooted[ranks - 1][1] == 10);
        });
    }

    TEST_CASE("a few MiB move through every collective") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            constexpr std::size_t BYTES = std::size_t{3} << 20;
            constexpr std::size_t COUNT = std::size_t{1} << 20;
            // A block of one letter per rank with a marker every few KiB, so a
            // view that starts anywhere else than its own offset differs.
            dstd::Vector<std::string> expected(ranks);
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                expected[rank].assign(BYTES, char('a' + rank));
                for (std::size_t at = 0; at < BYTES; at += 4099)
                    expected[rank][at] = char('A' + (at / 4099) % 26);
            }
            dstd::Vector<comm::Gathered> got(ranks);
            dstd::Vector<comm::Bytes> sent(ranks);
            dstd::Vector<dstd::Vector<std::int32_t>> sums(ranks);
            auto errors = run_ranks(
                rt, ranks,
                [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                    comm::Group group = communicator.world(rank);
                    const std::string& mine = expected[rank];
                    auto all_op = group.all_gather(bytes_of(mine));
                    auto all = co_await all_op;
                    if (!all) co_return std::move(all).error();
                    got[rank] = *all;
                    auto cast_op = group.broadcast(ranks - 1, bytes_of(mine));
                    auto cast = co_await cast_op;
                    if (!cast) co_return std::move(cast).error();
                    sent[rank] = *cast;
                    const dstd::Vector<std::int32_t> values(
                        COUNT, static_cast<std::int32_t>(rank + 1));
                    auto sum_op =
                        group.all_reduce(dstd::Span<const std::int32_t>(values),
                                         comm::ReduceOp::Sum);
                    auto sum = co_await sum_op;
                    if (!sum) co_return std::move(sum).error();
                    sums[rank] = *sum;
                    co_return {};
                });
            CHECK(all_ok(errors));
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                REQUIRE(got[rank].size() == ranks);
                for (std::uint32_t from = 0; from < ranks; ++from) {
                    CHECK(got[rank][from].size() == BYTES);
                    CHECK(std::memcmp(got[rank][from].data(),
                                      expected[from].data(), BYTES) == 0);
                }
                REQUIRE(sent[rank].size() == BYTES);
                CHECK(std::memcmp(sent[rank].view.data(),
                                  expected[ranks - 1].data(), BYTES) == 0);
                REQUIRE(sums[rank].size() == COUNT);
                CHECK(sums[rank][0] == std::int32_t(ranks * (ranks + 1) / 2));
                CHECK(sums[rank][COUNT - 1] ==
                      std::int32_t(ranks * (ranks + 1) / 2));
            }
        });
    }

    TEST_CASE("a result stays valid after the call, the ranks and the group") {
        for (const Binding binding : BINDINGS) {
            CAPTURE(binding_name(binding));
            Runtime rt(4);
            dstd::Vector<comm::Gathered> got(3);
            dstd::Vector<comm::Bytes> cast(3);
            {
                comm::Communicator communicator =
                    start(3, *rt.executor(), binding);
                auto errors = run_ranks(
                    rt, 3,
                    [&](std::uint32_t rank)
                        -> coro::CoroTask<dstd::Result<void>> {
                        comm::Group group = communicator.world(rank);
                        const std::string word = word_of(rank) + "!";
                        auto all_op = group.all_gather(bytes_of(word));
                        auto all = co_await all_op;
                        if (!all) co_return std::move(all).error();
                        got[rank] = *all;
                        const std::string tail = rank == 2 ? "tail" : "";
                        auto one_op = group.broadcast(2, bytes_of(tail));
                        auto one = co_await one_op;
                        if (!one) co_return std::move(one).error();
                        cast[rank] = *one;
                        co_return {};
                    });
                CHECK(all_ok(errors));
            }
            for (std::uint32_t rank = 0; rank < 3; ++rank) {
                REQUIRE(got[rank].size() == 3);
                for (std::uint32_t from = 0; from < 3; ++from)
                    CHECK(text_of(got[rank][from]) == word_of(from) + "!");
                CHECK(text_of(cast[rank].view) == "tail");
            }
        }
    }

    TEST_CASE("a view lives exactly as long as a copy of its result") {
        for (const Binding binding : BINDINGS) {
            CAPTURE(binding_name(binding));
            Runtime rt(4);
            comm::Communicator communicator = start(2, *rt.executor(), binding);
            dstd::Vector<comm::Gathered> got(2);
            auto errors = run_ranks(
                rt, 2,
                [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                    comm::Group group = communicator.world(rank);
                    const std::string word = "rank" + std::to_string(rank);
                    auto all_op = group.all_gather(bytes_of(word));
                    auto all = co_await all_op;
                    if (!all) co_return std::move(all).error();
                    got[rank] = *all;
                    co_return {};
                });
            CHECK(all_ok(errors));
            std::weak_ptr<const void> buffer = got[0].keep();
            REQUIRE_FALSE(buffer.expired());
            comm::Gathered second = got[1];
            got[0] = comm::Gathered{};
            got[1] = comm::Gathered{};
            // The copy keeps the one buffer, whoever else let go of it.
            CHECK_FALSE(buffer.expired());
            CHECK(text_of(second[0]) == "rank0");
            CHECK(text_of(second[1]) == "rank1");
            second = comm::Gathered{};
            CHECK(buffer.expired());
        }
    }

    TEST_CASE("a call that takes its bytes releases them when it ends") {
        for (const Binding binding : BINDINGS) {
            CAPTURE(binding_name(binding));
            Runtime rt(4);
            comm::Communicator communicator = start(2, *rt.executor(), binding);
            auto owner = std::make_shared<std::string>("owned by the caller");
            std::weak_ptr<std::string> watch = owner;
            auto errors = run_ranks(
                rt, 2,
                [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                    comm::Group group = communicator.world(rank);
                    comm::Bytes value;
                    if (rank == 0) {
                        value = comm::Bytes::borrowed(bytes_of(*owner), owner);
                    }
                    auto got_op = group.broadcast(0, std::move(value));
                    auto got = co_await got_op;
                    if (!got) co_return std::move(got).error();
                    CHECK(text_of(got->view) == "owned by the caller");
                    co_return {};
                });
            CHECK(all_ok(errors));
            owner.reset();
            CHECK(watch.expired());
        }
    }

    TEST_CASE("ranks that call different collectives all fail") {
        struct Mismatch {
            const char* name;
            const char* text;
        };
        const Mismatch cases[] = {{"call", "called"},
                                  {"root", "root"},
                                  {"element size", "element size"},
                                  {"count", "count"},
                                  {"reduce op", "reduce op"},
                                  {"reduce order", "reduce order"},
                                  {"root range", "is not below"}};
        for (const Binding binding : BINDINGS) {
            for (const Mismatch& mismatch : cases) {
                CAPTURE(binding_name(binding));
                CAPTURE(mismatch.name);
                Runtime rt(4);
                comm::Communicator communicator =
                    start(3, *rt.executor(), binding);
                const std::string which = mismatch.name;
                auto errors = run_ranks(
                    rt, 3,
                    [&](std::uint32_t rank)
                        -> coro::CoroTask<dstd::Result<void>> {
                        comm::Group group = communicator.world(rank);
                        const dstd::Vector<std::int32_t> values = {1, 2, 3};
                        const dstd::Vector<std::int64_t> wide = {1, 2, 3};
                        dstd::Result<void> outcome;
                        if (which == "call") {
                            if (rank == 0) {
                                auto got_op = group.all_gather(
                                    dstd::Span<const std::int32_t>(values));
                                auto got = co_await got_op;
                                if (!got) outcome = std::move(got).error();
                            } else {
                                auto got_op = group.all_reduce(
                                    std::int32_t{1}, comm::ReduceOp::Sum);
                                auto got = co_await got_op;
                                if (!got) outcome = std::move(got).error();
                            }
                        } else if (which == "root") {
                            auto got_op = group.broadcast(
                                rank == 0 ? 0u : 1u,
                                dstd::Span<const std::int32_t>(values));
                            auto got = co_await got_op;
                            if (!got) outcome = std::move(got).error();
                        } else if (which == "element size") {
                            if (rank == 0) {
                                auto got_op = group.broadcast(
                                    0, dstd::Span<const std::int32_t>(values));
                                auto got = co_await got_op;
                                if (!got) outcome = std::move(got).error();
                            } else {
                                auto got_op = group.broadcast(
                                    0, dstd::Span<const std::int64_t>(wide));
                                auto got = co_await got_op;
                                if (!got) outcome = std::move(got).error();
                            }
                        } else if (which == "count") {
                            const dstd::Span<const std::int32_t> span(
                                values.data(), rank == 0 ? 2 : 3);
                            auto got_op = group.all_gather(span);
                            auto got = co_await got_op;
                            if (!got) outcome = std::move(got).error();
                        } else if (which == "reduce op") {
                            auto got_op = group.all_reduce(
                                std::int32_t{1}, rank == 0
                                                     ? comm::ReduceOp::Sum
                                                     : comm::ReduceOp::Max);
                            auto got = co_await got_op;
                            if (!got) outcome = std::move(got).error();
                        } else if (which == "reduce order") {
                            comm::CallOpts opts;
                            if (rank != 0)
                                opts.reduce_order = comm::ReduceOrder::Any;
                            auto got_op = group.all_reduce(
                                std::int32_t{1}, comm::ReduceOp::Sum, opts);
                            auto got = co_await got_op;
                            if (!got) outcome = std::move(got).error();
                        } else {
                            auto got_op = group.broadcast(
                                3, dstd::Span<const std::int32_t>(values));
                            auto got = co_await got_op;
                            if (!got) outcome = std::move(got).error();
                        }
                        co_return outcome;
                    });
                for (const auto& error : errors) {
                    REQUIRE(error.has_value());
                    CHECK(has_text(*error, mismatch.text));
                }
            }
        }
    }

    TEST_CASE(
        "a deadline on one rank aborts the group and names who is missing") {
        for (const Binding binding : BINDINGS) {
            CAPTURE(binding_name(binding));
            Runtime rt(4);
            comm::Communicator communicator = start(3, *rt.executor(), binding);
            auto errors = run_ranks(
                rt, 3,
                [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                    if (rank == 2) co_return {};
                    comm::Group group = communicator.world(rank);
                    comm::CallOpts opts;
                    opts.deadline_ns = dftu_comm_test::deadline_after(
                        rank == 0 ? 40'000'000 : 400'000'000);
                    const dstd::Vector<std::int32_t> values = {1};
                    auto got_op = group.all_gather(
                        dstd::Span<const std::int32_t>(values), opts);
                    auto got = co_await got_op;
                    if (!got) co_return std::move(got).error();
                    co_return {};
                });
            REQUIRE(errors[0].has_value());
            REQUIRE(errors[1].has_value());
            CHECK(errors[0]->is(dstd::Condition::Timeout));
            CHECK(has_text(*errors[0], "missing ranks: 2"));
            CHECK(has_text(*errors[1], "aborted by rank 0"));
        }
    }

    TEST_CASE("a cancel aborts the group of a collective") {
        for (const Binding binding : BINDINGS) {
            CAPTURE(binding_name(binding));
            Runtime rt(4);
            comm::Communicator communicator = start(2, *rt.executor(), binding);
            std::atomic<bool> cancelled{false};
            auto errors = run_ranks(
                rt, 2,
                [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                    comm::Group group = communicator.world(rank);
                    const dstd::Vector<std::int32_t> values = {1};
                    if (rank == 1)
                        co_await dftu_comm_test::wait_flag(cancelled);
                    comm::Op<comm::Gathered> op = group.all_gather(
                        dstd::Span<const std::int32_t>(values));
                    if (rank == 0) {
                        op.cancel();
                        cancelled.store(true, std::memory_order_release);
                    }
                    auto got = co_await op;
                    if (!got) co_return std::move(got).error();
                    co_return {};
                });
            REQUIRE(errors[0].has_value());
            REQUIRE(errors[1].has_value());
            CHECK(errors[0]->is(dstd::Condition::Cancelled));
            CHECK(errors[1]->is(dstd::Condition::Cancelled));
            CHECK(has_text(*errors[1], "aborted by rank 0"));
        }
    }

    TEST_CASE("an empty group ends every collective with an empty value") {
        comm::Group empty;
        auto cast = empty.broadcast(0, std::int32_t{5}).wait();
        REQUIRE(cast.has_value());
        CHECK(*cast == 0);
        auto all = empty.all_gather(dstd::Span<const std::byte>()).wait();
        REQUIRE(all.has_value());
        CHECK(all->size() == 0);
    }

    TEST_CASE("a provider with its own instance per rank moves the table") {
        auto messaging = comm::backend::make_local_messaging();
        auto make_record = [](std::uint32_t flags) {
            comm::backend::Record record;
            record.flags = comm::backend::RECORD_TABLE | flags;
            return record;
        };
        auto contribution = [](const std::string& text) {
            auto owner = std::make_shared<std::string>(text);
            return comm::backend::Contribution{bytes_of(*owner), owner, {}};
        };
        auto fold_sum = [](std::byte* acc, const std::byte* in,
                           std::size_t bytes) {
            for (std::size_t at = 0; at + sizeof(std::int32_t) <= bytes;
                 at += sizeof(std::int32_t)) {
                std::int32_t left;
                std::int32_t right;
                std::memcpy(&left, acc + at, sizeof(left));
                std::memcpy(&right, in + at, sizeof(right));
                left += right;
                std::memcpy(acc + at, &left, sizeof(left));
            }
        };
        SUBCASE("every member that wants it gets the contributions") {
            auto zero = comm::backend::make_linear_collectives(messaging);
            auto one = comm::backend::make_linear_collectives(messaging);
            comm::backend::Table table0;
            comm::backend::Table table1;
            int ended = 0;
            auto done0 = [&](const comm::backend::SlotEnd& end) {
                table0 = end.table;
                ++ended;
            };
            auto done1 = [&](const comm::backend::SlotEnd& end) {
                table1 = end.table;
                ++ended;
            };
            const auto wants = make_record(comm::backend::RECORD_WANTS);
            CHECK(
                one->arrive(1, 0, 2, 1, wants, contribution("second"), done1));
            CHECK_FALSE(
                zero->arrive(1, 0, 2, 0, wants, contribution("first"), done0));
            CHECK(ended == 2);
            for (const auto* table : {&table0, &table1}) {
                REQUIRE(table->size() == 2);
                CHECK(text_of(table->at(0)) == "first");
                CHECK(text_of(table->at(1)) == "second");
            }
        }
        SUBCASE("a member that does not want it gets no table") {
            auto zero = comm::backend::make_linear_collectives(messaging);
            auto one = comm::backend::make_linear_collectives(messaging);
            comm::backend::Table table0;
            comm::backend::Table table1;
            int ended = 0;
            auto done0 = [&](const comm::backend::SlotEnd& end) {
                table0 = end.table;
                ++ended;
            };
            auto done1 = [&](const comm::backend::SlotEnd& end) {
                table1 = end.table;
                ++ended;
            };
            CHECK(one->arrive(1, 0, 2, 1, make_record(0),
                              contribution("second"), done1));
            CHECK_FALSE(zero->arrive(1, 0, 2, 0,
                                     make_record(comm::backend::RECORD_WANTS),
                                     contribution("first"), done0));
            CHECK(ended == 2);
            REQUIRE(table0.size() == 2);
            CHECK(text_of(table0.at(1)) == "second");
            CHECK(table1.size() == 0);
        }
        SUBCASE("members that combine fold in arrival order across instances") {
            auto zero = comm::backend::make_linear_collectives(messaging);
            auto one = comm::backend::make_linear_collectives(messaging);
            auto two = comm::backend::make_linear_collectives(messaging);
            comm::backend::Table table0;
            comm::backend::Table table1;
            comm::backend::Table table2;
            int ended = 0;
            auto done = [&ended](comm::backend::Table& into) {
                return [&into, &ended](const comm::backend::SlotEnd& end) {
                    into = end.table;
                    ++ended;
                };
            };
            const auto record = make_record(comm::backend::RECORD_WANTS |
                                            comm::backend::RECORD_COMBINE);
            auto number = [&](std::int32_t value) {
                auto owner = std::make_shared<std::int32_t>(value);
                comm::backend::Contribution made;
                made.bytes = std::as_bytes(
                    dstd::Span<const std::int32_t>(owner.get(), 1));
                made.keep = owner;
                made.combine = fold_sum;
                return made;
            };
            CHECK(two->arrive(7, 3, 3, 2, record, number(30), done(table2)));
            CHECK(zero->arrive(7, 3, 3, 0, record, number(10), done(table0)));
            CHECK_FALSE(
                one->arrive(7, 3, 3, 1, record, number(20), done(table1)));
            CHECK(ended == 3);
            for (comm::backend::Table* table : {&table0, &table1, &table2}) {
                REQUIRE(table->offsets.size() >= 2);
                std::int32_t total = 0;
                REQUIRE(table->at(0).size() == sizeof(total));
                std::memcpy(&total, table->at(0).data(), sizeof(total));
                CHECK(total == 60);
            }
        }
    }

    TEST_CASE("a borrowed payload keeps its block until it is delivered") {
        auto messaging = comm::backend::make_local_messaging();
        const comm::backend::MessageKey key{9, 0, 1, 5};
        auto block = std::make_shared<std::string>("borrowed bytes");
        std::weak_ptr<std::string> watch = block;
        int sent = 0;
        messaging->send(
            key, comm::backend::Payload::borrowed(bytes_of(*block), block),
            comm::backend::HOLD_EVERYTHING,
            [&sent](const dstd::Error*) { ++sent; });
        block.reset();
        // The message holds the block while it waits for a receive.
        CHECK_FALSE(watch.expired());
        std::string seen;
        messaging->recv(key,
                        [&seen](dstd::Span<const std::byte> bytes,
                                const dstd::Error*) { seen = text_of(bytes); });
        CHECK(seen == "borrowed bytes");
        CHECK(sent == 1);
        CHECK(watch.expired());
    }

    TEST_CASE("every collective works over a provider per rank") {
        for (const std::uint32_t ranks : {2u, 3u, 4u}) {
            CAPTURE(ranks);
            const std::uint32_t root = ranks - 1;
            Runtime rt(4);
            comm::Communicator communicator =
                start_per_rank(ranks, *rt.executor(), root);
            dstd::Vector<comm::Gathered> gathered(ranks);
            dstd::Vector<comm::Gathered> rooted(ranks);
            dstd::Vector<std::int64_t> sum(ranks);
            dstd::Vector<std::int64_t> sum_any(ranks);
            dstd::Vector<dstd::Vector<std::int64_t>> reduced(ranks);
            dstd::Vector<std::string> cast(ranks);
            dstd::Vector<dstd::Vector<std::int32_t>> piece(ranks);
            auto errors = run_ranks(
                rt, ranks,
                [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                    comm::Group group = communicator.world(rank);
                    auto barrier_op = group.barrier();
                    auto barrier = co_await barrier_op;
                    if (!barrier) co_return barrier;
                    const std::string word = word_of(rank);
                    auto all_op = group.all_gather(bytes_of(word));
                    auto all = co_await all_op;
                    if (!all) co_return std::move(all).error();
                    gathered[rank] = *all;
                    auto some_op = group.gather(root, bytes_of(word));
                    auto some = co_await some_op;
                    if (!some) co_return std::move(some).error();
                    rooted[rank] = *some;
                    const std::int64_t mine = rank + 1;
                    auto total_op = group.all_reduce(mine, comm::ReduceOp::Sum);
                    auto total = co_await total_op;
                    if (!total) co_return std::move(total).error();
                    sum[rank] = *total;
                    comm::CallOpts any;
                    any.reduce_order = comm::ReduceOrder::Any;
                    auto total_any_op =
                        group.all_reduce(mine, comm::ReduceOp::Sum, any);
                    auto total_any = co_await total_any_op;
                    if (!total_any) co_return std::move(total_any).error();
                    sum_any[rank] = *total_any;
                    const dstd::Vector<std::int64_t> values = {mine, 10};
                    auto one_op = group.reduce(
                        root, dstd::Span<const std::int64_t>(values),
                        comm::ReduceOp::Max);
                    auto one = co_await one_op;
                    if (!one) co_return std::move(one).error();
                    reduced[rank] = *one;
                    const std::string tail = rank == root ? "tail" : "";
                    auto sent_op = group.broadcast(root, bytes_of(tail));
                    auto sent = co_await sent_op;
                    if (!sent) co_return std::move(sent).error();
                    cast[rank] = text_of(sent->view);
                    dstd::Vector<std::int32_t> to_scatter;
                    if (rank == root)
                        for (std::uint32_t at = 0; at < ranks * 2; ++at)
                            to_scatter.push_back(static_cast<std::int32_t>(at));
                    auto part_op = group.scatter(
                        root, dstd::Span<const std::int32_t>(to_scatter));
                    auto part = co_await part_op;
                    if (!part) co_return std::move(part).error();
                    piece[rank] = *part;
                    co_return {};
                });
            CHECK(all_ok(errors));
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                REQUIRE(gathered[rank].size() == ranks);
                for (std::uint32_t from = 0; from < ranks; ++from)
                    CHECK(text_of(gathered[rank][from]) == word_of(from));
                CHECK(sum[rank] == std::int64_t(ranks) * (ranks + 1) / 2);
                CHECK(sum_any[rank] == sum[rank]);
                CHECK(cast[rank] == "tail");
                CHECK(piece[rank] ==
                      dstd::Vector<std::int32_t>{
                          static_cast<std::int32_t>(rank * 2),
                          static_cast<std::int32_t>(rank * 2 + 1)});
            }
            REQUIRE(rooted[root].size() == ranks);
            REQUIRE(reduced[root].size() == 2);
            CHECK(reduced[root][0] == ranks);
            CHECK(reduced[root][1] == 10);
        }
    }

    TEST_CASE("a member that did not ask for the table ends the call alone") {
        Runtime rt(4);
        comm::Communicator communicator = start_per_rank(2, *rt.executor(), 1);
        dstd::Vector<comm::Gathered> got(2);
        auto errors = run_ranks(
            rt, 2,
            [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                comm::Group group = communicator.world(rank);
                const dstd::Vector<std::int32_t> values = {1};
                auto result_op =
                    group.gather(0, dstd::Span<const std::int32_t>(values));
                auto result = co_await result_op;
                if (!result) co_return std::move(result).error();
                got[rank] = *result;
                co_return {};
            });
        CHECK(all_ok(errors));
        CHECK(got[1].size() == 0);
    }

    TEST_CASE("a user merge that calls the group does not block the provider") {
        for (const Binding binding : BINDINGS) {
            CAPTURE(binding_name(binding));
            Runtime rt(4);
            comm::Communicator communicator = start(3, *rt.executor(), binding);
            std::atomic<int> calls{0};
            dstd::Vector<std::int64_t> sum(3);
            auto errors = run_ranks(
                rt, 3,
                [&](std::uint32_t rank) -> coro::CoroTask<dstd::Result<void>> {
                    comm::Group group = communicator.world(rank);
                    comm::Group solo;
                    auto split_op = group.split(rank, 0);
                    auto split = co_await split_op;
                    if (!split) co_return std::move(split).error();
                    solo = *split;
                    comm::CallOpts opts;
                    opts.reduce_order = comm::ReduceOrder::Any;
                    const std::int64_t mine = rank + 1;
                    auto total_op = group.all_reduce(
                        mine,
                        [&solo, &calls](const std::int64_t& a,
                                        const std::int64_t& b) {
                            // A call of the same provider from inside the fold.
                            comm::Op<void> inner = solo.barrier();
                            (void)inner.wait();
                            calls.fetch_add(1);
                            return a + b;
                        },
                        opts);
                    auto total = co_await total_op;
                    if (!total) co_return std::move(total).error();
                    sum[rank] = *total;
                    co_return {};
                });
            CHECK(all_ok(errors));
            CHECK(calls.load() >= 2);
            for (const std::int64_t value : sum) CHECK(value == 6);
        }
    }

    TEST_CASE("a table a peer sent wrong ends the slot with an error") {
        struct Bad {
            const char* name;
            std::size_t size;
            std::uint64_t offsets[3];
        };
        // Two members with a table: the header holds 2 records of 40 bytes and
        // 3 offsets, padded to 128 bytes.
        const Bad cases[] = {{"short", 10, {0, 0, 0}},
                             {"first offset", 131, {1, 3, 3}},
                             {"offsets go back", 131, {0, 5, 3}},
                             {"offsets name too many bytes", 131, {0, 1, 900}}};
        for (const Bad& bad : cases) {
            CAPTURE(bad.name);
            auto messaging = comm::backend::make_local_messaging();
            auto instance = comm::backend::make_linear_collectives(messaging);
            comm::backend::Record record;
            record.flags =
                comm::backend::RECORD_TABLE | comm::backend::RECORD_WANTS;
            std::string why;
            int ended = 0;
            auto owner = std::make_shared<std::string>("x");
            comm::backend::Contribution contribution{
                bytes_of(*owner), owner, {}};
            CHECK(instance->arrive(1, 0, 2, 1, record, std::move(contribution),
                                   [&](const comm::backend::SlotEnd& end) {
                                       ++ended;
                                       if (end.why != nullptr)
                                           why =
                                               std::string(end.why->message());
                                   }));
            dstd::Vector<std::byte> wire(bad.size);
            if (bad.size >= 131) {
                std::memcpy(wire.data() + 80, bad.offsets, sizeof(bad.offsets));
            }
            messaging->send(
                comm::backend::MessageKey{
                    1, 0, 1,
                    comm::backend::message_tag(
                        comm::backend::TagPurpose::CollectiveTable, 0)},
                std::move(wire), comm::backend::HOLD_EVERYTHING,
                [](const dstd::Error*) {});
            CHECK(ended == 1);
            CHECK(why.find("a collective table") != std::string::npos);
        }
    }

    TEST_CASE("a cancelled fold starts no merge and drops its queue") {
        using comm::backend::ArrivalFold;
        auto held = [](std::int64_t value) {
            auto owner = std::make_shared<std::int64_t>(value);
            return ArrivalFold::Held::borrowed(
                std::as_bytes(dstd::Span<const std::int64_t>(owner.get(), 1)),
                owner);
        };
        int merges = 0;
        ArrivalFold fold;
        fold.set_combine_locked(
            [&merges](std::byte*, const std::byte*, std::size_t) { ++merges; });
        ArrivalFold::Batch batch;
        REQUIRE(fold.take_locked(held(1), batch));
        // Folding now, so the next two wait behind it.
        ArrivalFold::Batch unused;
        CHECK_FALSE(fold.take_locked(held(2), unused));
        CHECK_FALSE(fold.take_locked(held(3), unused));
        CHECK(fold.busy_locked());
        fold.cancel_locked();
        fold.apply(batch);
        CHECK_FALSE(fold.refill_locked(batch));
        CHECK_FALSE(fold.busy_locked());
        CHECK(merges == 0);
        // A batch taken before the cancel merges nothing after it.
        ArrivalFold other;
        other.set_combine_locked(
            [&merges](std::byte*, const std::byte*, std::size_t) { ++merges; });
        ArrivalFold::Batch two;
        REQUIRE(other.take_locked(held(1), two));
        two.items.push_back(held(2));
        two.items.push_back(held(3));
        other.cancel_locked();
        other.apply(two);
        CHECK(merges == 0);
    }

    TEST_CASE("a dropped slot lets no merge start") {
        auto collectives = comm::backend::make_local_collectives();
        std::atomic<int> merges{0};
        std::atomic<int> after_drop{0};
        std::atomic<bool> dropped{false};
        std::atomic<bool> in_merge{false};
        std::atomic<bool> release_merge{false};
        auto make = [&](std::int64_t value, bool slow) {
            auto owner = std::make_shared<std::int64_t>(value);
            comm::backend::Contribution made;
            made.bytes =
                std::as_bytes(dstd::Span<const std::int64_t>(owner.get(), 1));
            made.keep = owner;
            made.combine = [&, slow](std::byte* acc, const std::byte* in,
                                     std::size_t) {
                merges.fetch_add(1);
                if (dropped.load()) after_drop.fetch_add(1);
                if (slow) {
                    in_merge.store(true);
                    while (!release_merge.load()) std::this_thread::yield();
                }
                std::int64_t left;
                std::int64_t right;
                std::memcpy(&left, acc, sizeof(left));
                std::memcpy(&right, in, sizeof(right));
                left += right;
                std::memcpy(acc, &left, sizeof(left));
            };
            return made;
        };
        comm::backend::Record record;
        record.flags = comm::backend::RECORD_TABLE |
                       comm::backend::RECORD_WANTS |
                       comm::backend::RECORD_COMBINE;
        auto done = [](const comm::backend::SlotEnd&) {};
        collectives->arrive(1, 0, 4, 0, record, make(1, true), done);
        std::thread folder([&] {
            collectives->arrive(1, 0, 4, 1, record, make(2, false), done);
        });
        REQUIRE(dftu_comm_test::spin_until([&] { return in_merge.load(); }));
        // Two more queue behind the merge that runs.
        collectives->arrive(1, 0, 4, 2, record, make(3, false), done);
        collectives->arrive(1, 0, 4, 3, record, make(4, false), done);
        collectives->drop(1, 0, nullptr);
        dropped.store(true);
        release_merge.store(true);
        folder.join();
        CHECK(after_drop.load() == 0);
        CHECK(merges.load() == 1);
    }

    TEST_CASE("a message that follows the release of its group parks nothing") {
        auto messaging = comm::backend::make_local_messaging();
        const comm::backend::MessageKey key{7, 0, 1, 5};
        messaging->release(7);
        const dstd::Error* sent_error = nullptr;
        std::string sent_text;
        std::string recv_text;
        messaging->send(key, dstd::Vector<std::byte>(4),
                        comm::backend::HOLD_EVERYTHING,
                        [&](const dstd::Error* why) {
                            if (why != nullptr)
                                sent_text = std::string(why->message());
                            sent_error = why;
                        });
        messaging->recv(
            key, [&](dstd::Span<const std::byte>, const dstd::Error* why) {
                if (why != nullptr) recv_text = std::string(why->message());
            });
        std::string any_text;
        messaging->recv_any_source(
            key, 1,
            [&](std::uint32_t, dstd::Span<const std::byte>,
                const dstd::Error* why) {
                if (why != nullptr) any_text = std::string(why->message());
            });
        std::byte buffer[4];
        std::string into_text;
        messaging->recv_into(
            key, 1, dstd::Span<std::byte>(buffer, 4),
            [&](std::uint32_t, std::size_t, const dstd::Error* why) {
                if (why != nullptr) into_text = std::string(why->message());
            });
        messaging->close(key);
        messaging->abort(7, dstd::Error{dstd::Condition::Cancelled, "late"});
        messaging->release(7);
        REQUIRE(sent_error != nullptr);
        CHECK(sent_text.find("released") != std::string::npos);
        CHECK(recv_text.find("released") != std::string::npos);
        CHECK(any_text.find("released") != std::string::npos);
        CHECK(into_text.find("released") != std::string::npos);
        const auto counts = comm::backend::local_counts(*messaging);
        CHECK(counts.streams == 0);
        CHECK(counts.parked == 0);
        CHECK(counts.ended == 0);
    }

    TEST_CASE("released groups stay a few ranges and every one refuses") {
        auto messaging = comm::backend::make_local_messaging();
        constexpr std::uint64_t GROUPS = 1000;
        constexpr std::uint64_t KEPT = 500;
        // Every id from 1 to GROUPS but KEPT, in an order that is not
        // monotonic.
        std::uint64_t at = 0;
        for (std::uint64_t step = 0; step < GROUPS; ++step) {
            at = (at + 337) % GROUPS;
            const std::uint64_t id = at + 1;
            if (id == KEPT) continue;
            messaging->release(id);
        }
        const auto counts = comm::backend::local_counts(*messaging);
        CHECK(counts.released_ranges == 2);
        CHECK(counts.ended == 0);
        int refused = 0;
        for (std::uint64_t id = 1; id <= GROUPS; ++id) {
            if (id == KEPT) continue;
            messaging->send(comm::backend::MessageKey{id, 0, 1, 1},
                            dstd::Vector<std::byte>(1),
                            comm::backend::HOLD_EVERYTHING,
                            [&refused](const dstd::Error* why) {
                                if (why != nullptr) ++refused;
                            });
        }
        CHECK(refused == static_cast<int>(GROUPS - 1));
        CHECK(comm::backend::local_counts(*messaging).streams == 0);
        // The id that was never released still works.
        int kept_sent = 0;
        messaging->send(comm::backend::MessageKey{KEPT, 0, 1, 1},
                        dstd::Vector<std::byte>(1),
                        comm::backend::HOLD_EVERYTHING,
                        [&kept_sent](const dstd::Error* why) {
                            if (why == nullptr) ++kept_sent;
                        });
        CHECK(kept_sent == 1);
        CHECK(comm::backend::local_counts(*messaging).parked == 1);
    }
}
