// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <comm/collective_runs.h>
#include <comm/support.h>
#include <dftracer/utils/comm/backend/local.h>
#include <dftracer/utils/comm/codec.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/comm/op.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>

using namespace dftracer::utils;

namespace {

using dftu_comm_test::all_ok;
using dftu_comm_test::run_ranks;
using dftu_comm_test::start_processes;

using Task = coro::CoroTask<dstd::Result<void>>;
using Word = dstd::SharedPtr<const dstd::String>;

/// The string codec, counting each call.
struct CountingCodec {
    static inline std::atomic<int> serialized{0};
    static inline std::atomic<int> deserialized{0};

    static void reset() {
        serialized.store(0);
        deserialized.store(0);
    }

    std::size_t serialized_size(dstd::StringView value) const noexcept {
        return comm::StringCodec{}.serialized_size(value);
    }

    dstd::Result<std::size_t> serialize(dstd::StringView value,
                                        dstd::Span<std::byte> out) const {
        serialized.fetch_add(1);
        return comm::StringCodec{}.serialize(value, out);
    }

    dstd::Result<dstd::String> deserialize(
        dstd::Span<const std::byte> bytes) const {
        deserialized.fetch_add(1);
        return comm::StringCodec{}.deserialize(bytes);
    }
};

static_assert(comm::Codec<CountingCodec, dstd::String>);

/// The string codec, refusing to deserialize the text "bad".
struct PickyCodec {
    std::size_t serialized_size(dstd::StringView value) const noexcept {
        return comm::StringCodec{}.serialized_size(value);
    }

    dstd::Result<std::size_t> serialize(dstd::StringView value,
                                        dstd::Span<std::byte> out) const {
        return comm::StringCodec{}.serialize(value, out);
    }

    dstd::Result<dstd::String> deserialize(
        dstd::Span<const std::byte> bytes) const {
        auto made = comm::StringCodec{}.deserialize(bytes);
        if (made && *made == dstd::String("bad")) {
            return dstd::Unexpected(dstd::Error::from_format<"refused bad">(
                dstd::Condition::InvalidArgument));
        }
        return made;
    }
};

static_assert(comm::Codec<PickyCodec, dstd::String>);

dstd::String word_of(std::uint32_t rank) {
    return dstd::String(("w" + dstd::to_string(rank)).c_str());
}

dstd::String piece_of(std::uint32_t from, std::uint32_t to) {
    return dstd::String(
        (dstd::to_string(from) + ">" + dstd::to_string(to)).c_str());
}

comm::Communicator start_mixed(Runtime& rt, dstd::Vector<std::uint32_t> map) {
    const std::uint32_t ranks = static_cast<std::uint32_t>(map.size());
    return start_processes(
        ranks, *rt.executor(), std::move(map),
        [](const auto&) { return comm::backend::make_local_collectives(); });
}

dstd::Vector<std::uint32_t> processes(
    std::initializer_list<std::uint32_t> list) {
    return dstd::Vector<std::uint32_t>(list.begin(), list.end());
}

}  // namespace

TEST_SUITE("CommObjectProcesses") {
    TEST_CASE("one process serializes nothing and shares every object") {
        CountingCodec::reset();
        Runtime rt(4);
        const auto map = processes({0, 0, 0, 0});
        comm::Communicator communicator = start_mixed(rt, map);
        dstd::Vector<Word> cast(4);
        dstd::Vector<dstd::Vector<Word>> everyone(4);
        auto errors = run_ranks(rt, 4, [&](std::uint32_t rank) -> Task {
            comm::Group group = communicator.world(rank);
            auto a_op =
                group.broadcast_object(1, word_of(rank), CountingCodec{});
            auto a = co_await a_op;
            if (!a) co_return std::move(a).error();
            cast[rank] = *a;
            auto b_op = group.all_gather_object(word_of(rank), CountingCodec{});
            auto b = co_await b_op;
            if (!b) co_return std::move(b).error();
            everyone[rank] = *b;
            co_return {};
        });
        CHECK(all_ok(errors));
        CHECK(CountingCodec::serialized.load() == 0);
        CHECK(CountingCodec::deserialized.load() == 0);
        for (std::uint32_t rank = 0; rank < 4; ++rank) {
            CHECK(*cast[rank] == word_of(1));
            CHECK(cast[rank].get() == cast[0].get());
            CHECK(everyone[rank][2].get() == everyone[0][2].get());
        }
    }

    TEST_CASE(
        "a broadcast serializes once and decodes once per other process") {
        for (const std::uint32_t root : {0u, 1u, 2u, 3u}) {
            CAPTURE(root);
            CountingCodec::reset();
            Runtime rt(4);
            comm::Communicator communicator =
                start_mixed(rt, processes({0, 0, 1, 1}));
            dstd::Vector<Word> cast(4);
            auto errors = run_ranks(rt, 4, [&](std::uint32_t rank) -> Task {
                comm::Group group = communicator.world(rank);
                auto got_op = group.broadcast_object(root, word_of(rank),
                                                     CountingCodec{});
                auto got = co_await got_op;
                if (!got) co_return std::move(got).error();
                cast[rank] = *got;
                co_return {};
            });
            CHECK(all_ok(errors));
            CHECK(CountingCodec::serialized.load() == 1);
            CHECK(CountingCodec::deserialized.load() == 2);
            for (std::uint32_t rank = 0; rank < 4; ++rank)
                CHECK(*cast[rank] == word_of(root));
            CHECK(cast[0].get() == cast[1].get());
            CHECK(cast[2].get() == cast[3].get());
            CHECK(cast[0].get() != cast[2].get());
        }
    }

    TEST_CASE("a broadcast over three processes decodes once in each other") {
        CountingCodec::reset();
        Runtime rt(4);
        comm::Communicator communicator =
            start_mixed(rt, processes({0, 1, 1, 2, 2}));
        dstd::Vector<Word> cast(5);
        auto errors = run_ranks(rt, 5, [&](std::uint32_t rank) -> Task {
            comm::Group group = communicator.world(rank);
            auto got_op =
                group.broadcast_object(1, word_of(rank), CountingCodec{});
            auto got = co_await got_op;
            if (!got) co_return std::move(got).error();
            cast[rank] = *got;
            co_return {};
        });
        CHECK(all_ok(errors));
        CHECK(CountingCodec::serialized.load() == 1);
        CHECK(CountingCodec::deserialized.load() == 3);
        for (std::uint32_t rank = 0; rank < 5; ++rank)
            CHECK(*cast[rank] == word_of(1));
        CHECK(cast[1].get() == cast[2].get());
        CHECK(cast[3].get() == cast[4].get());
        CHECK(cast[0].get() != cast[1].get());
        CHECK(cast[3].get() != cast[1].get());
        CHECK(cast[3].get() != cast[0].get());
    }

    TEST_CASE("a gather serializes only the values of other processes") {
        CountingCodec::reset();
        Runtime rt(4);
        comm::Communicator communicator =
            start_mixed(rt, processes({0, 0, 1, 1}));
        dstd::Vector<dstd::Vector<Word>> got(4);
        auto errors = run_ranks(rt, 4, [&](std::uint32_t rank) -> Task {
            comm::Group group = communicator.world(rank);
            auto got_op =
                group.gather_object(0, word_of(rank), CountingCodec{});
            auto made = co_await got_op;
            if (!made) co_return std::move(made).error();
            got[rank] = *made;
            co_return {};
        });
        CHECK(all_ok(errors));
        CHECK(CountingCodec::serialized.load() == 2);
        CHECK(CountingCodec::deserialized.load() == 4);
        REQUIRE(got[0].size() == 4);
        for (std::uint32_t from = 0; from < 4; ++from)
            CHECK(*got[0][from] == word_of(from));
        for (std::uint32_t rank = 1; rank < 4; ++rank) CHECK(got[rank].empty());
    }

    TEST_CASE("a gather at the root of the other side serializes the same") {
        CountingCodec::reset();
        Runtime rt(4);
        comm::Communicator communicator =
            start_mixed(rt, processes({0, 0, 1, 1}));
        dstd::Vector<dstd::Vector<Word>> got(4);
        auto errors = run_ranks(rt, 4, [&](std::uint32_t rank) -> Task {
            comm::Group group = communicator.world(rank);
            auto got_op =
                group.gather_object(3, word_of(rank), CountingCodec{});
            auto made = co_await got_op;
            if (!made) co_return std::move(made).error();
            got[rank] = *made;
            co_return {};
        });
        CHECK(all_ok(errors));
        CHECK(CountingCodec::serialized.load() == 2);
        CHECK(CountingCodec::deserialized.load() == 4);
        REQUIRE(got[3].size() == 4);
        for (std::uint32_t from = 0; from < 4; ++from)
            CHECK(*got[3][from] == word_of(from));
    }

    TEST_CASE("an all_gather decodes each value once per other process") {
        CountingCodec::reset();
        Runtime rt(4);
        comm::Communicator communicator =
            start_mixed(rt, processes({0, 0, 1, 1}));
        dstd::Vector<dstd::Vector<Word>> everyone(4);
        auto errors = run_ranks(rt, 4, [&](std::uint32_t rank) -> Task {
            comm::Group group = communicator.world(rank);
            auto got_op =
                group.all_gather_object(word_of(rank), CountingCodec{});
            auto got = co_await got_op;
            if (!got) co_return std::move(got).error();
            everyone[rank] = *got;
            co_return {};
        });
        CHECK(all_ok(errors));
        CHECK(CountingCodec::serialized.load() == 4);
        CHECK(CountingCodec::deserialized.load() == 8);
        for (std::uint32_t rank = 0; rank < 4; ++rank) {
            REQUIRE(everyone[rank].size() == 4);
            for (std::uint32_t from = 0; from < 4; ++from)
                CHECK(*everyone[rank][from] == word_of(from));
        }
        for (std::uint32_t from = 0; from < 4; ++from) {
            CHECK(everyone[0][from].get() == everyone[1][from].get());
            CHECK(everyone[2][from].get() == everyone[3][from].get());
        }
        CHECK(everyone[0][2].get() != everyone[2][2].get());
        CHECK(everyone[2][0].get() != everyone[0][0].get());
    }

    TEST_CASE("an all_to_all serializes only the pieces for other processes") {
        CountingCodec::reset();
        Runtime rt(4);
        comm::Communicator communicator =
            start_mixed(rt, processes({0, 0, 1, 1}));
        dstd::Vector<dstd::Vector<Word>> swapped(4);
        auto errors = run_ranks(rt, 4, [&](std::uint32_t rank) -> Task {
            comm::Group group = communicator.world(rank);
            dstd::Vector<dstd::String> sends;
            for (std::uint32_t dest = 0; dest < 4; ++dest)
                sends.push_back(piece_of(rank, dest));
            auto got_op =
                group.all_to_all_object(std::move(sends), CountingCodec{});
            auto got = co_await got_op;
            if (!got) co_return std::move(got).error();
            swapped[rank] = *got;
            co_return {};
        });
        CHECK(all_ok(errors));
        CHECK(CountingCodec::serialized.load() == 8);
        CHECK(CountingCodec::deserialized.load() == 16);
        for (std::uint32_t rank = 0; rank < 4; ++rank) {
            REQUIRE(swapped[rank].size() == 4);
            for (std::uint32_t from = 0; from < 4; ++from)
                CHECK(*swapped[rank][from] == piece_of(from, rank));
        }
    }

    TEST_CASE("a group with no other process in it never serializes") {
        CountingCodec::reset();
        Runtime rt(4);
        comm::Communicator communicator =
            start_mixed(rt, processes({0, 0, 1, 1}));
        auto errors = run_ranks(rt, 4, [&](std::uint32_t rank) -> Task {
            comm::Group world = communicator.world(rank);
            auto child_op = world.split(rank / 2, rank);
            auto child = co_await child_op;
            if (!child) co_return std::move(child).error();
            auto got_op =
                child->all_gather_object(word_of(rank), CountingCodec{});
            auto got = co_await got_op;
            if (!got) co_return std::move(got).error();
            co_return {};
        });
        CHECK(all_ok(errors));
        CHECK(CountingCodec::serialized.load() == 0);
        CHECK(CountingCodec::deserialized.load() == 0);
    }

    TEST_CASE(
        "a value one process cannot decode fails the call of every rank") {
        for (const int call : {0, 1, 2, 3}) {
            CAPTURE(call);
            Runtime rt(4);
            comm::Communicator communicator =
                start_mixed(rt, processes({0, 0, 1, 1}));
            auto errors = run_ranks(rt, 4, [&](std::uint32_t rank) -> Task {
                comm::Group group = communicator.world(rank);
                const dstd::String mine = (call == 0 && rank == 0) ||
                                                  (call == 1 && rank == 0) ||
                                                  (call == 2 && rank == 2)
                                              ? dstd::String("bad")
                                              : word_of(rank);
                dstd::Result<void> outcome;
                if (call == 0) {
                    auto got_op = group.broadcast_object(0, mine, PickyCodec{});
                    auto got = co_await got_op;
                    if (!got) outcome = std::move(got).error();
                } else if (call == 1) {
                    auto got_op = group.all_gather_object(mine, PickyCodec{});
                    auto got = co_await got_op;
                    if (!got) outcome = std::move(got).error();
                } else if (call == 2) {
                    auto got_op = group.gather_object(0, mine, PickyCodec{});
                    auto got = co_await got_op;
                    if (!got) outcome = std::move(got).error();
                } else {
                    dstd::Vector<dstd::String> sends;
                    for (std::uint32_t dest = 0; dest < 4; ++dest)
                        sends.push_back(rank == 0 && dest == 2
                                            ? dstd::String("bad")
                                            : piece_of(rank, dest));
                    auto got_op =
                        group.all_to_all_object(std::move(sends), PickyCodec{});
                    auto got = co_await got_op;
                    if (!got) outcome = std::move(got).error();
                }
                co_return outcome;
            });
            for (std::uint32_t rank = 0; rank < 4; ++rank) {
                REQUIRE(errors[rank].has_value());
                CHECK(dftu_comm_test::has_text(*errors[rank], "refused bad"));
                CHECK(
                    dftu_comm_test::has_text(*errors[rank], "aborted by rank"));
            }
        }
    }
}
