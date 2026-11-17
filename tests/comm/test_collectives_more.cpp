// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <comm/collective_runs.h>
#include <comm/support.h>
#include <dftracer/utils/comm/backend/linear_collectives.h>
#include <dftracer/utils/comm/backend/local.h>
#include <dftracer/utils/comm/codec.h>
#include <dftracer/utils/comm/collective.h>
#include <dftracer/utils/comm/group.h>
#include <dftracer/utils/comm/op.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
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
using dftu_comm_test::run_ranks;
using dftu_comm_test::start;
using dftu_comm_test::text_of;

using Task = coro::CoroTask<dstd::Result<void>>;

/// The text rank @p from sends to rank @p to; its size is 0 to 2.
std::string piece_of(std::uint32_t from, std::uint32_t to) {
    return std::string((from + to) % 3, char('a' + from * 4 + to));
}

/// One linear instance per rank, as separate processes have them. Only the Done
/// of the driver ends a call, except for a call of personal contributions,
/// whose Done waits for the table of every rank: each instance serves its own
/// rank only.
class PerRankAll : public comm::backend::Collectives {
   public:
    PerRankAll(dstd::SharedPtr<comm::backend::Messaging> messaging,
               std::uint32_t ranks, std::uint32_t driver_rank)
        : driver_(driver_rank) {
        for (std::uint32_t rank = 0; rank < ranks; ++rank)
            instances_.push_back(
                comm::backend::make_linear_collectives(messaging));
    }

    bool arrive(std::uint64_t group_id, std::uint64_t seq,
                std::uint32_t members, std::uint32_t rank,
                const comm::backend::Record& record,
                comm::backend::Contribution&& contribution,
                Done done) override {
        if ((record.flags & comm::backend::RECORD_PERSONAL) == 0) {
            if (rank != driver_) done = [](const comm::backend::SlotEnd&) {};
            return instances_[rank]->arrive(group_id, seq, members, rank,
                                            record, std::move(contribution),
                                            std::move(done));
        }
        std::shared_ptr<Merge> merge;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto& entry = merges_[{group_id, seq}];
            if (!entry) {
                entry = std::make_shared<Merge>();
                entry->tables.resize(members);
            }
            merge = entry;
        }
        if (rank == driver_) {
            std::lock_guard<std::mutex> lock(merge->mutex);
            merge->driver = std::move(done);
        }
        const std::uint32_t driver = driver_;
        Done mine = [merge, rank, members,
                     driver](const comm::backend::SlotEnd& end) {
            const bool whole =
                end.why == nullptr && end.seen.empty() && !end.tables.empty();
            {
                std::lock_guard<std::mutex> lock(merge->mutex);
                if (!whole) {
                    if (rank != driver) return;
                } else {
                    merge->tables[rank] = end.tables[rank];
                    if (++merge->got != members) return;
                }
            }
            comm::backend::SlotEnd merged = end;
            if (whole)
                merged.tables = dstd::Span<const comm::backend::Table>(
                    merge->tables.data(), merge->tables.size());
            merge->driver(merged);
        };
        return instances_[rank]->arrive(group_id, seq, members, rank, record,
                                        std::move(contribution),
                                        std::move(mine));
    }

    void drop(std::uint64_t group_id, std::uint64_t seq,
              comm::backend::Arrival* arrived) override {
        for (auto& instance : instances_)
            instance->drop(group_id, seq, arrived);
    }

    void release(std::uint64_t group_id) override {
        for (auto& instance : instances_) instance->release(group_id);
    }

   private:
    struct Merge {
        std::mutex mutex;
        Done driver;
        dstd::Vector<comm::backend::Table> tables;
        std::uint32_t got = 0;
    };

    std::uint32_t driver_;
    dstd::Vector<dstd::SharedPtr<comm::backend::Collectives>> instances_;
    std::mutex mutex_;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::shared_ptr<Merge>>
        merges_;
};

comm::Communicator start_per_rank_all(std::uint32_t ranks,
                                      coro::Executor& executor,
                                      std::uint32_t driver) {
    return dftu_comm_test::start_processes(
        ranks, executor, dftu_comm_test::process_per_rank(ranks),
        [ranks, driver](const auto& messaging) {
            return dstd::make_shared<PerRankAll>(messaging, ranks, driver);
        });
}

/// The provider of one process of a run that has a rank per process: it ends a
/// slot at the first arrival with the table it was given, as the one rank it
/// serves, and holds no other rank.
class OneRank : public comm::backend::Collectives {
   public:
    OneRank(dstd::Vector<dstd::Vector<std::byte>> entries, bool personal)
        : entries_(std::move(entries)), personal_(personal) {}

    bool arrive(std::uint64_t, std::uint64_t, std::uint32_t members,
                std::uint32_t rank, const comm::backend::Record& record,
                comm::backend::Contribution&&, Done done) override {
        dstd::Vector<comm::backend::Record> records(members, record);
        auto buffer = dstd::make_shared<comm::backend::TableBuffer>();
        buffer->offsets.push_back(0);
        for (const auto& entry : entries_) {
            buffer->data.insert(buffer->data.end(), entry.begin(), entry.end());
            buffer->offsets.push_back(buffer->data.size());
        }
        const comm::backend::Table table =
            comm::backend::make_table(std::move(buffer));
        dstd::Vector<comm::backend::Table> tables;
        if (personal_) {
            tables.resize(members);
            tables[rank] = table;
        }
        const comm::backend::SlotEnd end{
            dstd::Span<const comm::backend::Record>(records.data(),
                                                    records.size()),
            {},
            rank,
            nullptr,
            personal_ ? comm::backend::Table{} : table,
            dstd::Span<const comm::backend::Table>(tables.data(),
                                                   tables.size())};
        done(end);
        return false;
    }

    void drop(std::uint64_t, std::uint64_t, comm::backend::Arrival*) override {}
    void release(std::uint64_t) override {}

   private:
    dstd::Vector<dstd::Vector<std::byte>> entries_;
    bool personal_ = false;
};

template <class T>
dstd::Vector<std::byte> bytes_of_values(std::initializer_list<T> values) {
    dstd::Vector<std::byte> out(values.size() * sizeof(T));
    std::size_t at = 0;
    for (const T& value : values) {
        std::memcpy(out.data() + at, &value, sizeof(T));
        at += sizeof(T);
    }
    return out;
}

comm::Communicator start_one_rank(coro::Executor& executor,
                                  dstd::Vector<dstd::Vector<std::byte>> entries,
                                  bool personal) {
    auto started = comm::Communicator::start_local(2, executor);
    REQUIRE(started.has_value());
    comm::Communicator communicator = std::move(*started);
    comm::detail::Hooks::set_collectives(
        communicator, dstd::make_shared<OneRank>(std::move(entries), personal));
    return communicator;
}

struct Row {
    std::uint32_t key;
    std::uint32_t seq;
    std::int64_t from;
};

struct Person {
    std::string name;
    std::int32_t age = 0;
};

/// A name behind its length, then the age; refuses an age of 99 to serialize
/// and an age of 13 to deserialize.
struct PersonCodec {
    static inline std::atomic<int> serialized{0};

    std::size_t serialized_size(const Person& person) const {
        return sizeof(std::uint32_t) + person.name.size() +
               sizeof(std::int32_t);
    }

    dstd::Result<std::size_t> serialize(const Person& person,
                                        dstd::Span<std::byte> out) const {
        if (out.size() != serialized_size(person)) return bad("size");
        if (person.age == 99) return bad("serialize refused");
        serialized.fetch_add(1);
        const std::uint32_t size =
            static_cast<std::uint32_t>(person.name.size());
        std::memcpy(out.data(), &size, sizeof(size));
        if (size != 0)
            std::memcpy(out.data() + sizeof(size), person.name.data(), size);
        std::memcpy(out.data() + sizeof(size) + size, &person.age,
                    sizeof(std::int32_t));
        return out.size();
    }

    dstd::Result<Person> deserialize(dstd::Span<const std::byte> in) const {
        std::uint32_t size = 0;
        if (in.size() < sizeof(size) + sizeof(std::int32_t))
            return bad("short");
        std::memcpy(&size, in.data(), sizeof(size));
        if (in.size() != sizeof(size) + size + sizeof(std::int32_t))
            return bad("size");
        Person person;
        person.name = text_of(in.subspan(sizeof(size), size));
        std::memcpy(&person.age, in.data() + sizeof(size) + size,
                    sizeof(std::int32_t));
        if (person.age == 13) return bad("unlucky age");
        return person;
    }

    static dstd::Unexpected<dstd::Error> bad(const char* why) {
        return dstd::Unexpected(dstd::Error::from_format<"bad person: {}">(
            dstd::Condition::InvalidArgument, dstd::StringView(why)));
    }
};

static_assert(comm::Codec<PersonCodec, Person>);

/// A codec whose size and writes break the contract by the age of the person:
/// -1 names the largest size, -2 one that leaves no room for the head, -3 names
/// and writes nothing, -4 writes one byte fewer than it names.
struct EdgeCodec {
    std::size_t serialized_size(const Person& person) const {
        if (person.age == -1) return std::numeric_limits<std::size_t>::max();
        if (person.age == -2)
            return std::numeric_limits<std::size_t>::max() - 4;
        if (person.age == -3) return 0;
        return PersonCodec{}.serialized_size(person);
    }

    dstd::Result<std::size_t> serialize(const Person& person,
                                        dstd::Span<std::byte> out) const {
        if (person.age == -3) return std::size_t{0};
        auto done = PersonCodec{}.serialize(person, out);
        if (done && person.age == -4) return *done - 1;
        return done;
    }

    dstd::Result<Person> deserialize(dstd::Span<const std::byte> in) const {
        return PersonCodec{}.deserialize(in);
    }
};

static_assert(comm::Codec<EdgeCodec, Person>);

struct Number {
    std::int32_t value = 0;
};

struct NumberCodec {
    std::size_t serialized_size(const Number&) const {
        return sizeof(std::int32_t);
    }

    dstd::Result<std::size_t> serialize(const Number& number,
                                        dstd::Span<std::byte> out) const {
        std::memcpy(out.data(), &number.value, sizeof(std::int32_t));
        return out.size();
    }

    dstd::Result<Number> deserialize(dstd::Span<const std::byte> in) const {
        Number number;
        std::memcpy(&number.value, in.data(), sizeof(std::int32_t));
        return number;
    }
};

static_assert(comm::Codec<NumberCodec, Number>);

}  // namespace

TEST_SUITE("CommCollectivesMore") {
    TEST_CASE("an all_to_all of equal blocks sends block d to rank d") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            for (const std::size_t block :
                 {std::size_t{0}, std::size_t{1}, std::size_t{3}}) {
                CAPTURE(block);
                dstd::Vector<comm::Gathered> got(ranks);
                auto errors =
                    run_ranks(rt, ranks, [&](std::uint32_t rank) -> Task {
                        comm::Group group = communicator.world(rank);
                        dstd::Vector<std::int32_t> values;
                        for (std::uint32_t dest = 0; dest < ranks; ++dest)
                            for (std::size_t at = 0; at < block; ++at)
                                values.push_back(static_cast<std::int32_t>(
                                    rank * 100 + dest * 10 + at));
                        auto out_op = group.all_to_all(
                            dstd::Span<const std::int32_t>(values));
                        auto out = co_await out_op;
                        if (!out) co_return std::move(out).error();
                        got[rank] = *out;
                        co_return {};
                    });
                CHECK(all_ok(errors));
                for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                    REQUIRE(got[rank].size() == ranks);
                    for (std::uint32_t from = 0; from < ranks; ++from) {
                        const auto view = got[rank].as<std::int32_t>(from);
                        REQUIRE(view.size() == block);
                        for (std::size_t at = 0; at < block; ++at)
                            CHECK(view[at] == static_cast<std::int32_t>(
                                                  from * 100 + rank * 10 + at));
                    }
                }
            }
        });
    }

    TEST_CASE("an all_to_all of uneven byte blocks keeps every block apart") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            dstd::Vector<comm::Gathered> got(ranks);
            auto errors = run_ranks(rt, ranks, [&](std::uint32_t rank) -> Task {
                comm::Group group = communicator.world(rank);
                dstd::Vector<std::string> texts;
                for (std::uint32_t dest = 0; dest < ranks; ++dest)
                    texts.push_back(piece_of(rank, dest));
                dstd::Vector<dstd::Span<const std::byte>> blocks;
                for (const std::string& text : texts)
                    blocks.push_back(bytes_of(text));
                auto out_op = group.all_to_all(
                    dstd::Span<const dstd::Span<const std::byte>>(
                        blocks.data(), blocks.size()));
                auto out = co_await out_op;
                if (!out) co_return std::move(out).error();
                got[rank] = *out;
                co_return {};
            });
            CHECK(all_ok(errors));
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                REQUIRE(got[rank].size() == ranks);
                for (std::uint32_t from = 0; from < ranks; ++from)
                    CHECK(text_of(got[rank][from]) == piece_of(from, rank));
            }
        });
    }

    TEST_CASE(
        "an all_to_all that the ranks do not agree on fails on every rank") {
        struct Case {
            const char* name;
            const char* text;
        };
        const Case cases[] = {{"split", "do not split"},
                              {"blocks", "blocks for a group"},
                              {"count", "count"},
                              {"layout", "element size"},
                              {"call", "called"}};
        for (const Binding binding : BINDINGS) {
            for (const Case& one : cases) {
                CAPTURE(binding_name(binding));
                CAPTURE(one.name);
                Runtime rt(4);
                comm::Communicator communicator =
                    start(3, *rt.executor(), binding);
                const std::string which = one.name;
                auto errors = run_ranks(rt, 3, [&](std::uint32_t rank) -> Task {
                    comm::Group group = communicator.world(rank);
                    const dstd::Vector<std::int32_t> four = {1, 2, 3, 4};
                    const dstd::Vector<std::int32_t> three = {1, 2, 3};
                    const dstd::Vector<std::int32_t> six = {1, 2, 3, 4, 5, 6};
                    const std::string text = "x";
                    const dstd::Vector<dstd::Span<const std::byte>> two = {
                        bytes_of(text), bytes_of(text)};
                    const dstd::Vector<dstd::Span<const std::byte>> blocks = {
                        bytes_of(text), bytes_of(text), bytes_of(text)};
                    dstd::Result<void> outcome;
                    if (which == "split") {
                        auto got_op = group.all_to_all(
                            dstd::Span<const std::int32_t>(four));
                        auto got = co_await got_op;
                        if (!got) outcome = std::move(got).error();
                    } else if (which == "blocks") {
                        auto got_op = group.all_to_all(
                            dstd::Span<const dstd::Span<const std::byte>>(
                                two.data(), two.size()));
                        auto got = co_await got_op;
                        if (!got) outcome = std::move(got).error();
                    } else if (which == "count") {
                        auto got_op =
                            group.all_to_all(dstd::Span<const std::int32_t>(
                                rank == 0 ? three : six));
                        auto got = co_await got_op;
                        if (!got) outcome = std::move(got).error();
                    } else if (which == "layout") {
                        if (rank == 0) {
                            auto got_op = group.all_to_all(
                                dstd::Span<const std::int32_t>(three));
                            auto got = co_await got_op;
                            if (!got) outcome = std::move(got).error();
                        } else {
                            auto got_op = group.all_to_all(
                                dstd::Span<const dstd::Span<const std::byte>>(
                                    blocks.data(), blocks.size()));
                            auto got = co_await got_op;
                            if (!got) outcome = std::move(got).error();
                        }
                    } else {
                        if (rank == 0) {
                            auto got_op = group.all_to_all(
                                dstd::Span<const std::int32_t>(three));
                            auto got = co_await got_op;
                            if (!got) outcome = std::move(got).error();
                        } else {
                            auto got_op = group.all_gather(
                                dstd::Span<const std::int32_t>(three));
                            auto got = co_await got_op;
                            if (!got) outcome = std::move(got).error();
                        }
                    }
                    co_return outcome;
                });
                for (std::uint32_t rank = 0; rank < 3; ++rank) {
                    REQUIRE(errors[rank].has_value());
                    CHECK(has_text(*errors[rank], one.text));
                }
            }
        }
    }

    TEST_CASE("exchange_by groups rows by source rank and keeps their order") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            auto rows_of = [](std::uint32_t rank) {
                dstd::Vector<Row> rows;
                for (std::uint32_t at = 0; at < 9; ++at)
                    rows.push_back(Row{rank * 3 + at * 5, at,
                                       static_cast<std::int64_t>(rank)});
                return rows;
            };
            dstd::Vector<comm::Gathered> got(ranks);
            auto errors = run_ranks(rt, ranks, [&](std::uint32_t rank) -> Task {
                comm::Group group = communicator.world(rank);
                const dstd::Vector<Row> rows = rows_of(rank);
                const std::uint32_t count = ranks;
                auto out_op = group.exchange_by(
                    dstd::Span<const Row>(rows),
                    [count](const Row& row) { return row.key % count; });
                auto out = co_await out_op;
                if (!out) co_return std::move(out).error();
                got[rank] = *out;
                co_return {};
            });
            CHECK(all_ok(errors));
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                REQUIRE(got[rank].size() == ranks);
                for (std::uint32_t from = 0; from < ranks; ++from) {
                    dstd::Vector<Row> expected;
                    for (const Row& row : rows_of(from))
                        if (row.key % ranks == rank) expected.push_back(row);
                    const auto view = got[rank].as<Row>(from);
                    REQUIRE(view.size() == expected.size());
                    for (std::size_t at = 0; at < view.size(); ++at) {
                        CHECK(view[at].seq == expected[at].seq);
                        CHECK(view[at].from == expected[at].from);
                    }
                }
            }
        });
    }

    TEST_CASE("a row sent to a rank outside the group fails every rank") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            auto errors = run_ranks(rt, ranks, [&](std::uint32_t rank) -> Task {
                comm::Group group = communicator.world(rank);
                const dstd::Vector<Row> rows = {Row{0, 0, 0}, Row{1, 1, 0}};
                const std::uint32_t count = ranks;
                const std::uint32_t me = rank;
                auto out_op = group.exchange_by(
                    dstd::Span<const Row>(rows), [count, me](const Row& row) {
                        return me == 0 && row.seq == 1 ? count : 0u;
                    });
                auto out = co_await out_op;
                if (!out) co_return std::move(out).error();
                co_return {};
            });
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                REQUIRE(errors[rank].has_value());
                CHECK(has_text(*errors[rank], "is not below the group size"));
            }
        });
    }

    TEST_CASE("reduce_scatter gives rank r block r of the merge") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            const std::size_t block = 2;
            auto value = [](std::uint32_t rank, std::size_t at) {
                return static_cast<std::int64_t>(rank + 1) +
                       static_cast<std::int64_t>(at) * 10;
            };
            dstd::Vector<dstd::Vector<std::int64_t>> equal(ranks);
            dstd::Vector<dstd::Vector<std::int64_t>> counted(ranks);
            dstd::Vector<dstd::Vector<std::int64_t>> picked(ranks);
            dstd::Vector<dstd::Vector<std::int64_t>> any(ranks);
            dstd::Vector<std::size_t> counts;
            std::size_t total = 0;
            for (std::uint32_t dest = 0; dest < ranks; ++dest) {
                counts.push_back(dest == 1 ? 0 : dest + 1);
                total += counts.back();
            }
            auto errors = run_ranks(rt, ranks, [&](std::uint32_t rank) -> Task {
                comm::Group group = communicator.world(rank);
                dstd::Vector<std::int64_t> values;
                for (std::size_t at = 0; at < block * ranks; ++at)
                    values.push_back(value(rank, at));
                auto one_op =
                    group.reduce_scatter(dstd::Span<const std::int64_t>(values),
                                         comm::ReduceOp::Sum);
                auto one = co_await one_op;
                if (!one) co_return std::move(one).error();
                equal[rank] = *one;
                comm::CallOpts any_order;
                any_order.reduce_order = comm::ReduceOrder::Any;
                auto loose_op =
                    group.reduce_scatter(dstd::Span<const std::int64_t>(values),
                                         comm::ReduceOp::Sum, any_order);
                auto loose = co_await loose_op;
                if (!loose) co_return std::move(loose).error();
                any[rank] = *loose;
                dstd::Vector<std::int64_t> uneven;
                for (std::size_t at = 0; at < total; ++at)
                    uneven.push_back(value(rank, at));
                auto split_op = group.reduce_scatter(
                    dstd::Span<const std::int64_t>(uneven),
                    dstd::Span<const std::size_t>(counts), comm::ReduceOp::Max);
                auto split = co_await split_op;
                if (!split) co_return std::move(split).error();
                counted[rank] = *split;
                auto fold = [](const std::int64_t& left,
                               const std::int64_t& right) {
                    return left * 3 + right;
                };
                auto custom_op = group.reduce_scatter(
                    dstd::Span<const std::int64_t>(values), fold);
                auto custom = co_await custom_op;
                if (!custom) co_return std::move(custom).error();
                picked[rank] = *custom;
                co_return {};
            });
            CHECK(all_ok(errors));
            std::size_t begin = 0;
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                REQUIRE(equal[rank].size() == block);
                REQUIRE(any[rank].size() == block);
                REQUIRE(picked[rank].size() == block);
                for (std::size_t at = 0; at < block; ++at) {
                    const std::size_t index = rank * block + at;
                    std::int64_t sum = 0;
                    std::int64_t folded = value(0, index);
                    for (std::uint32_t from = 0; from < ranks; ++from) {
                        sum += value(from, index);
                        if (from != 0) folded = folded * 3 + value(from, index);
                    }
                    CHECK(equal[rank][at] == sum);
                    CHECK(any[rank][at] == sum);
                    CHECK(picked[rank][at] == folded);
                }
                REQUIRE(counted[rank].size() == counts[rank]);
                for (std::size_t at = 0; at < counts[rank]; ++at)
                    CHECK(counted[rank][at] == value(ranks - 1, begin + at));
                begin += counts[rank];
            }
        });
    }

    TEST_CASE("a float reduce_scatter and scan fold in rank order every run") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            auto value = [](std::uint32_t rank) {
                const float pool[] = {1e8f, 1.0f, -1e8f, 3.0f};
                return pool[rank % 4];
            };
            for (int round = 0; round < 5; ++round) {
                dstd::Vector<float> scattered(ranks);
                dstd::Vector<float> prefix(ranks);
                auto errors =
                    run_ranks(rt, ranks, [&](std::uint32_t rank) -> Task {
                        comm::Group group = communicator.world(rank);
                        const dstd::Vector<float> values(ranks, value(rank));
                        auto one_op = group.reduce_scatter(
                            dstd::Span<const float>(values),
                            comm::ReduceOp::Sum);
                        auto one = co_await one_op;
                        if (!one) co_return std::move(one).error();
                        scattered[rank] = (*one)[0];
                        auto two_op =
                            group.scan(value(rank), comm::ReduceOp::Sum);
                        auto two = co_await two_op;
                        if (!two) co_return std::move(two).error();
                        prefix[rank] = *two;
                        co_return {};
                    });
                CHECK(all_ok(errors));
                float running = 0;
                for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                    running = rank == 0 ? value(0) : running + value(rank);
                    CHECK(prefix[rank] == running);
                    CHECK(scattered[rank] == prefix[ranks - 1]);
                }
            }
        });
    }

    TEST_CASE(
        "reduce_scatter counts that the ranks do not share fail them all") {
        struct Case {
            const char* name;
            const char* text;
        };
        const Case cases[] = {{"differ", "different counts"},
                              {"sum", "do not split"},
                              {"overflow", "do not split"},
                              {"equal", "do not split"}};
        for (const Binding binding : BINDINGS) {
            for (const comm::ReduceOrder order :
                 {comm::ReduceOrder::Rank, comm::ReduceOrder::Any}) {
                for (const Case& one : cases) {
                    CAPTURE(binding_name(binding));
                    CAPTURE(one.name);
                    Runtime rt(4);
                    comm::Communicator communicator =
                        start(3, *rt.executor(), binding);
                    const std::string which = one.name;
                    auto errors =
                        run_ranks(rt, 3, [&](std::uint32_t rank) -> Task {
                            comm::Group group = communicator.world(rank);
                            const dstd::Vector<std::int32_t> values = {1, 2, 3,
                                                                       4};
                            const dstd::Vector<std::size_t> first = {2, 1, 1};
                            const dstd::Vector<std::size_t> second = {1, 2, 1};
                            const dstd::Vector<std::size_t> wrong = {2, 2, 2};
                            const std::size_t huge =
                                std::numeric_limits<std::size_t>::max();
                            const dstd::Vector<std::size_t> wrap = {huge, 2, 3};
                            comm::CallOpts opts;
                            opts.reduce_order = order;
                            dstd::Result<void> outcome;
                            if (which == "equal") {
                                auto got_op = group.reduce_scatter(
                                    dstd::Span<const std::int32_t>(values),
                                    comm::ReduceOp::Sum, opts);
                                auto got = co_await got_op;
                                if (!got) outcome = std::move(got).error();
                            } else {
                                const auto& counts =
                                    which == "sum" ? wrong
                                    : which == "overflow"
                                        ? wrap
                                        : (rank == 1 ? second : first);
                                auto got_op = group.reduce_scatter(
                                    dstd::Span<const std::int32_t>(values),
                                    dstd::Span<const std::size_t>(counts),
                                    comm::ReduceOp::Sum, opts);
                                auto got = co_await got_op;
                                if (!got) outcome = std::move(got).error();
                            }
                            co_return outcome;
                        });
                    for (std::uint32_t rank = 0; rank < 3; ++rank) {
                        REQUIRE(errors[rank].has_value());
                        CHECK(has_text(*errors[rank], one.text));
                    }
                }
            }
        }
    }

    TEST_CASE("a key that does not fit 32 bits is not a rank") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            auto errors = run_ranks(rt, ranks, [&](std::uint32_t rank) -> Task {
                comm::Group group = communicator.world(rank);
                const dstd::Vector<Row> rows = {Row{0, 0, 0}};
                auto out_op = group.exchange_by(
                    dstd::Span<const Row>(rows), [](const Row&) {
                        return std::uint64_t{(std::uint64_t{1} << 32) + 1};
                    });
                auto out = co_await out_op;
                if (!out) co_return std::move(out).error();
                co_return {};
            });
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                REQUIRE(errors[rank].has_value());
                CHECK(has_text(*errors[rank], "is not below the group size"));
            }
        });
    }

    TEST_CASE("scan and exclusive_scan merge the prefix in rank order") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            dstd::Vector<dstd::Vector<std::int64_t>> inclusive(ranks);
            dstd::Vector<dstd::Vector<std::int64_t>> exclusive(ranks);
            dstd::Vector<std::int64_t> digits(ranks);
            dstd::Vector<dstd::Optional<std::int64_t>> before(ranks);
            dstd::Vector<dstd::Optional<std::int64_t>> before_digits(ranks);
            auto errors = run_ranks(rt, ranks, [&](std::uint32_t rank) -> Task {
                comm::Group group = communicator.world(rank);
                const std::int64_t mine = rank + 1;
                const dstd::Vector<std::int64_t> values = {mine, mine * 10};
                auto a_op = group.scan(dstd::Span<const std::int64_t>(values),
                                       comm::ReduceOp::Sum);
                auto a = co_await a_op;
                if (!a) co_return std::move(a).error();
                inclusive[rank] = *a;
                auto b_op =
                    group.exclusive_scan(dstd::Span<const std::int64_t>(values),
                                         comm::ReduceOp::Max);
                auto b = co_await b_op;
                if (!b) co_return std::move(b).error();
                exclusive[rank] = *b;
                auto join = [](const std::int64_t& left,
                               const std::int64_t& right) {
                    return left * 10 + right;
                };
                auto c_op = group.scan(mine, join);
                auto c = co_await c_op;
                if (!c) co_return std::move(c).error();
                digits[rank] = *c;
                auto d_op = group.exclusive_scan(mine, comm::ReduceOp::Sum);
                auto d = co_await d_op;
                if (!d) co_return std::move(d).error();
                before[rank] = *d;
                auto e_op = group.exclusive_scan(mine, join);
                auto e = co_await e_op;
                if (!e) co_return std::move(e).error();
                before_digits[rank] = *e;
                co_return {};
            });
            CHECK(all_ok(errors));
            std::int64_t sum = 0;
            std::int64_t number = 0;
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                const std::int64_t mine = rank + 1;
                if (rank == 0) {
                    CHECK(exclusive[rank].empty());
                    CHECK_FALSE(before[rank].has_value());
                    CHECK_FALSE(before_digits[rank].has_value());
                } else {
                    REQUIRE(exclusive[rank].size() == 2);
                    CHECK(exclusive[rank][0] == rank);
                    CHECK(exclusive[rank][1] == std::int64_t{rank} * 10);
                    REQUIRE(before[rank].has_value());
                    CHECK(*before[rank] == sum);
                    REQUIRE(before_digits[rank].has_value());
                    CHECK(*before_digits[rank] == number);
                }
                sum += mine;
                number = number * 10 + mine;
                REQUIRE(inclusive[rank].size() == 2);
                CHECK(inclusive[rank][0] == sum);
                CHECK(inclusive[rank][1] == sum * 10);
                CHECK(digits[rank] == number);
            }
        });
    }

    TEST_CASE("a scan whose ranks differ in kind, count or op fails them all") {
        for (const Binding binding : BINDINGS) {
            for (const int which : {0, 1, 2}) {
                CAPTURE(binding_name(binding));
                CAPTURE(which);
                Runtime rt(4);
                comm::Communicator communicator =
                    start(3, *rt.executor(), binding);
                auto errors = run_ranks(rt, 3, [&](std::uint32_t rank) -> Task {
                    comm::Group group = communicator.world(rank);
                    const dstd::Vector<std::int32_t> two = {1, 2};
                    const dstd::Vector<std::int32_t> one = {1};
                    dstd::Result<void> outcome;
                    if (which == 0) {
                        if (rank == 0) {
                            auto got_op =
                                group.scan(dstd::Span<const std::int32_t>(one),
                                           comm::ReduceOp::Sum);
                            auto got = co_await got_op;
                            if (!got) outcome = std::move(got).error();
                        } else {
                            auto got_op = group.exclusive_scan(
                                dstd::Span<const std::int32_t>(one),
                                comm::ReduceOp::Sum);
                            auto got = co_await got_op;
                            if (!got) outcome = std::move(got).error();
                        }
                    } else if (which == 1) {
                        auto got_op = group.scan(dstd::Span<const std::int32_t>(
                                                     rank == 0 ? two : one),
                                                 comm::ReduceOp::Sum);
                        auto got = co_await got_op;
                        if (!got) outcome = std::move(got).error();
                    } else {
                        auto got_op =
                            group.scan(dstd::Span<const std::int32_t>(one),
                                       rank == 0 ? comm::ReduceOp::Sum
                                                 : comm::ReduceOp::Max);
                        auto got = co_await got_op;
                        if (!got) outcome = std::move(got).error();
                    }
                    co_return outcome;
                });
                const char* text[] = {"called", "count", "reduce op"};
                for (std::uint32_t rank = 0; rank < 3; ++rank) {
                    REQUIRE(errors[rank].has_value());
                    CHECK(has_text(*errors[rank], text[which]));
                }
            }
        }
    }

    TEST_CASE("neighbors fixes a graph and neighbor_exchange moves along it") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            for (const int shape : {0, 1, 2}) {
                CAPTURE(shape);
                auto out_of = [&](std::uint32_t rank) {
                    dstd::Vector<std::uint32_t> out;
                    if (shape == 0) {
                        out.push_back((rank + 1) % ranks);
                    } else if (shape == 1 && rank == 0) {
                        for (std::uint32_t to = ranks; to-- > 1;)
                            out.push_back(to);
                    } else if (shape == 1) {
                        out.push_back(0);
                    }
                    return out;
                };
                auto text = [](std::uint32_t from, std::uint32_t to,
                               int round) {
                    return round == 1 && (from + to) % 2 == 0
                               ? std::string()
                               : std::string(from + 1 + round, char('a' + to));
                };
                dstd::Vector<dstd::Vector<std::uint32_t>> in(ranks);
                dstd::Vector<dstd::Vector<std::string>> first(ranks);
                dstd::Vector<dstd::Vector<std::string>> second(ranks);
                dstd::Vector<dstd::Vector<std::uint32_t>> typed(ranks);
                auto errors =
                    run_ranks(rt, ranks, [&](std::uint32_t rank) -> Task {
                        comm::Group group = communicator.world(rank);
                        const dstd::Vector<std::uint32_t> out = out_of(rank);
                        auto made_op = group.neighbors(
                            dstd::Span<const std::uint32_t>(out));
                        auto made = co_await made_op;
                        if (!made) co_return std::move(made).error();
                        comm::Neighborhood hood = *made;
                        const auto in_ranks = hood.in_neighbors();
                        for (const std::uint32_t from : in_ranks)
                            in[rank].push_back(from);
                        for (int round = 0; round < 2; ++round) {
                            dstd::Vector<std::string> texts;
                            for (const std::uint32_t to : out)
                                texts.push_back(text(rank, to, round));
                            dstd::Vector<dstd::Span<const std::byte>> blocks;
                            for (const std::string& one : texts)
                                blocks.push_back(bytes_of(one));
                            auto got_op = hood.neighbor_exchange(
                                dstd::Span<const dstd::Span<const std::byte>>(
                                    blocks.data(), blocks.size()));
                            auto got = co_await got_op;
                            if (!got) co_return std::move(got).error();
                            for (std::uint32_t at = 0; at < got->size(); ++at)
                                (round == 0 ? first : second)[rank].push_back(
                                    text_of((*got)[at]));
                        }
                        dstd::Vector<std::uint32_t> numbers;
                        for (const std::uint32_t to : out)
                            numbers.push_back(rank * 100 + to);
                        auto sent_op = hood.neighbor_exchange(
                            dstd::Span<const std::uint32_t>(numbers));
                        auto sent = co_await sent_op;
                        if (!sent) co_return std::move(sent).error();
                        for (std::uint32_t at = 0; at < sent->size(); ++at)
                            typed[rank].push_back(
                                sent->as<std::uint32_t>(at)[0]);
                        co_return {};
                    });
                CHECK(all_ok(errors));
                for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                    dstd::Vector<std::uint32_t> expected;
                    for (std::uint32_t from = 0; from < ranks; ++from)
                        for (const std::uint32_t to : out_of(from))
                            if (to == rank) expected.push_back(from);
                    CHECK(in[rank] == expected);
                    REQUIRE(first[rank].size() == expected.size());
                    REQUIRE(second[rank].size() == expected.size());
                    REQUIRE(typed[rank].size() == expected.size());
                    for (std::size_t at = 0; at < expected.size(); ++at) {
                        CHECK(first[rank][at] == text(expected[at], rank, 0));
                        CHECK(second[rank][at] == text(expected[at], rank, 1));
                        CHECK(typed[rank][at] == expected[at] * 100 + rank);
                    }
                }
            }
        });
    }

    TEST_CASE("a graph that is not valid fails neighbors on every rank") {
        for (const Binding binding : BINDINGS) {
            for (const std::uint32_t ranks : {1u, 2u, 3u}) {
                for (const bool duplicate : {true, false}) {
                    CAPTURE(binding_name(binding));
                    CAPTURE(ranks);
                    CAPTURE(duplicate);
                    Runtime rt(4);
                    comm::Communicator communicator =
                        start(ranks, *rt.executor(), binding);
                    auto errors =
                        run_ranks(rt, ranks, [&](std::uint32_t rank) -> Task {
                            comm::Group group = communicator.world(rank);
                            dstd::Vector<std::uint32_t> out;
                            if (rank == ranks - 1) {
                                if (duplicate)
                                    out = {0, 0};
                                else
                                    out = {ranks};
                            }
                            auto made_op = group.neighbors(
                                dstd::Span<const std::uint32_t>(out));
                            auto made = co_await made_op;
                            if (!made) co_return std::move(made).error();
                            co_return {};
                        });
                    for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                        REQUIRE(errors[rank].has_value());
                        CHECK(has_text(*errors[rank],
                                       duplicate ? "twice" : "is not below"));
                    }
                }
            }
        }
    }

    TEST_CASE(
        "a neighbor_exchange with the wrong block count fails every rank") {
        for_each_run([](Runtime& rt, comm::Communicator& communicator,
                        std::uint32_t ranks) {
            auto errors = run_ranks(rt, ranks, [&](std::uint32_t rank) -> Task {
                comm::Group group = communicator.world(rank);
                const dstd::Vector<std::uint32_t> out = {(rank + 1) % ranks};
                auto made_op =
                    group.neighbors(dstd::Span<const std::uint32_t>(out));
                auto made = co_await made_op;
                if (!made) co_return std::move(made).error();
                comm::Neighborhood hood = *made;
                dstd::Vector<std::uint32_t> values = {1};
                if (rank == 0) values.push_back(2);
                auto got_op = hood.neighbor_exchange(
                    dstd::Span<const std::uint32_t>(values));
                auto got = co_await got_op;
                if (!got) co_return std::move(got).error();
                co_return {};
            });
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                REQUIRE(errors[rank].has_value());
                CHECK(has_text(*errors[rank], "out-neighbors"));
            }
        });
    }

    TEST_CASE("the object forms round trip a value through a codec") {
        using Shared = dstd::SharedPtr<const Person>;
        for (const Binding binding : BINDINGS) {
            for (const std::uint32_t ranks : {1u, 2u, 3u, 4u}) {
                CAPTURE(binding_name(binding));
                CAPTURE(ranks);
                PersonCodec::serialized.store(0);
                Runtime rt(4);
                comm::Communicator communicator =
                    start(ranks, *rt.executor(), binding);
                const std::uint32_t root = ranks - 1;
                dstd::Vector<dstd::Vector<Shared>> everyone(ranks);
                dstd::Vector<dstd::Vector<Shared>> rooted(ranks);
                dstd::Vector<Shared> cast(ranks);
                dstd::Vector<dstd::Vector<Shared>> swapped(ranks);
                auto errors =
                    run_ranks(rt, ranks, [&](std::uint32_t rank) -> Task {
                        comm::Group group = communicator.world(rank);
                        const Person mine{"rank" + std::to_string(rank),
                                          static_cast<std::int32_t>(rank + 20)};
                        auto a_op =
                            group.all_gather_object(mine, PersonCodec{});
                        auto a = co_await a_op;
                        if (!a) co_return std::move(a).error();
                        everyone[rank] = *a;
                        auto b_op =
                            group.gather_object(root, mine, PersonCodec{});
                        auto b = co_await b_op;
                        if (!b) co_return std::move(b).error();
                        rooted[rank] = *b;
                        auto c_op =
                            group.broadcast_object(root, mine, PersonCodec{});
                        auto c = co_await c_op;
                        if (!c) co_return std::move(c).error();
                        cast[rank] = *c;
                        dstd::Vector<Person> sends;
                        for (std::uint32_t dest = 0; dest < ranks; ++dest)
                            sends.push_back(Person{
                                std::to_string(rank) + ">" +
                                    std::to_string(dest),
                                static_cast<std::int32_t>(100 + rank * 10 +
                                                          dest)});
                        auto d_op = group.all_to_all_object(std::move(sends),
                                                            PersonCodec{});
                        auto d = co_await d_op;
                        if (!d) co_return std::move(d).error();
                        swapped[rank] = *d;
                        co_return {};
                    });
                CHECK(all_ok(errors));
                for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                    REQUIRE(everyone[rank].size() == ranks);
                    for (std::uint32_t from = 0; from < ranks; ++from) {
                        CHECK(everyone[rank][from]->name ==
                              "rank" + std::to_string(from));
                        CHECK(everyone[rank][from]->age ==
                              static_cast<std::int32_t>(from + 20));
                    }
                    CHECK(rooted[rank].size() == (rank == root ? ranks : 0u));
                    CHECK(cast[rank]->name == "rank" + std::to_string(root));
                    REQUIRE(swapped[rank].size() == ranks);
                    for (std::uint32_t from = 0; from < ranks; ++from) {
                        CHECK(swapped[rank][from]->name ==
                              std::to_string(from) + ">" +
                                  std::to_string(rank));
                        CHECK(
                            swapped[rank][from]->age ==
                            static_cast<std::int32_t>(100 + from * 10 + rank));
                    }
                }
                if (binding == Binding::Local || ranks == 1) {
                    CHECK(PersonCodec::serialized.load() == 0);
                    for (std::uint32_t rank = 1; rank < ranks; ++rank) {
                        CHECK(everyone[rank][0].get() == everyone[0][0].get());
                        CHECK(cast[rank].get() == cast[0].get());
                    }
                } else {
                    CHECK(PersonCodec::serialized.load() > 0);
                    for (std::uint32_t rank = 1; rank < ranks; ++rank)
                        CHECK(everyone[rank][0].get() != everyone[0][0].get());
                }
            }
        }
    }

    TEST_CASE(
        "an object that does not deserialize fails its rank and aborts the "
        "group") {
        for (const Binding binding : BINDINGS) {
            for (const std::uint32_t ranks : {2u, 3u, 4u}) {
                CAPTURE(binding_name(binding));
                CAPTURE(ranks);
                Runtime rt(4);
                comm::Communicator communicator =
                    start(ranks, *rt.executor(), binding);
                auto errors =
                    run_ranks(rt, ranks, [&](std::uint32_t rank) -> Task {
                        comm::Group group = communicator.world(rank);
                        dstd::Vector<Person> sends;
                        for (std::uint32_t dest = 0; dest < ranks; ++dest)
                            sends.push_back(
                                Person{"x", rank == 0 && dest == 1 ? 13 : 1});
                        auto got_op = group.all_to_all_object(std::move(sends),
                                                              PersonCodec{});
                        auto got = co_await got_op;
                        if (!got) co_return std::move(got).error();
                        if (rank == 1 && binding == Binding::Linear)
                            co_return {};
                        auto later_op = group.barrier();
                        auto later = co_await later_op;
                        co_return later;
                    });
                if (binding == Binding::Local) {
                    CHECK(all_ok(errors));
                    continue;
                }
                for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                    REQUIRE(errors[rank].has_value());
                    CHECK(has_text(*errors[rank],
                                   "could not deserialize the object of "
                                   "rank 0"));
                    CHECK(has_text(*errors[rank], "unlucky age"));
                    CHECK(has_text(*errors[rank], "aborted by rank 0"));
                }
            }
        }
    }

    TEST_CASE("an object that every rank cannot deserialize fails them all") {
        for (const Binding binding : BINDINGS) {
            for (const std::uint32_t ranks : {1u, 2u, 3u, 4u}) {
                CAPTURE(binding_name(binding));
                CAPTURE(ranks);
                Runtime rt(4);
                comm::Communicator communicator =
                    start(ranks, *rt.executor(), binding);
                auto errors =
                    run_ranks(rt, ranks, [&](std::uint32_t rank) -> Task {
                        comm::Group group = communicator.world(rank);
                        const Person mine{"x", rank == 0 ? 13 : 1};
                        auto got_op =
                            group.broadcast_object(0, mine, PersonCodec{});
                        auto got = co_await got_op;
                        if (!got) co_return std::move(got).error();
                        co_return {};
                    });
                if (binding == Binding::Local || ranks == 1) {
                    CHECK(all_ok(errors));
                    continue;
                }
                for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                    REQUIRE(errors[rank].has_value());
                    CHECK(has_text(*errors[rank], "unlucky age"));
                }
            }
        }
    }

    TEST_CASE("a codec that cannot serialize aborts the group for every rank") {
        for (const int which : {0, 1, 2}) {
            for (const std::uint32_t ranks : {2u, 3u, 4u}) {
                CAPTURE(which);
                CAPTURE(ranks);
                Runtime rt(4);
                comm::Communicator communicator =
                    start(ranks, *rt.executor(), Binding::Linear);
                auto errors =
                    run_ranks(rt, ranks, [&](std::uint32_t rank) -> Task {
                        comm::Group group = communicator.world(rank);
                        const Person mine{"x", rank == 0 ? 99 : 1};
                        dstd::Result<void> outcome;
                        if (which == 0) {
                            auto got_op =
                                group.all_gather_object(mine, PersonCodec{});
                            auto got = co_await got_op;
                            if (!got) outcome = std::move(got).error();
                        } else if (which == 1) {
                            auto got_op =
                                group.broadcast_object(0, mine, PersonCodec{});
                            auto got = co_await got_op;
                            if (!got) outcome = std::move(got).error();
                        } else {
                            dstd::Vector<Person> sends(ranks, mine);
                            auto got_op = group.all_to_all_object(
                                std::move(sends), PersonCodec{});
                            auto got = co_await got_op;
                            if (!got) outcome = std::move(got).error();
                        }
                        co_return outcome;
                    });
                for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                    REQUIRE(errors[rank].has_value());
                    CHECK(has_text(*errors[rank], "serialize refused"));
                }
            }
        }
    }

    TEST_CASE("a codec that breaks its size contract aborts the group") {
        struct Case {
            const char* name;
            const char* text;
        };
        const Case cases[] = {{"short", "serialized_size named"},
                              {"huge gather", "cannot allocate"},
                              {"huge broadcast", "cannot allocate"},
                              {"huge sum", "more bytes than fit"},
                              {"huge block", "do not fit one block"},
                              {"short blocks", "serialized_size named"}};
        for (const Case& one : cases) {
            for (const std::uint32_t ranks : {2u, 3u}) {
                CAPTURE(one.name);
                CAPTURE(ranks);
                Runtime rt(4);
                comm::Communicator communicator =
                    start(ranks, *rt.executor(), Binding::Linear);
                const std::string which = one.name;
                auto errors =
                    run_ranks(rt, ranks, [&](std::uint32_t rank) -> Task {
                        comm::Group group = communicator.world(rank);
                        const bool bad = rank == 0;
                        auto edge = [&](std::int32_t age) {
                            return Person{"x", bad ? age : 1};
                        };
                        dstd::Result<void> outcome;
                        if (which == "short" || which == "huge gather") {
                            auto got_op = group.all_gather_object(
                                edge(which == "short" ? -4 : -1), EdgeCodec{});
                            auto got = co_await got_op;
                            if (!got) outcome = std::move(got).error();
                        } else if (which == "huge broadcast") {
                            auto got_op = group.broadcast_object(0, edge(-1),
                                                                 EdgeCodec{});
                            auto got = co_await got_op;
                            if (!got) outcome = std::move(got).error();
                        } else {
                            dstd::Vector<Person> sends(ranks, Person{"x", 1});
                            if (bad) {
                                if (which == "huge sum") {
                                    for (std::uint32_t at = 1;
                                         at < ranks && at < 3; ++at)
                                        sends[at].age = -1;
                                } else if (which == "huge block") {
                                    sends[1].age = -2;
                                    for (std::uint32_t at = 2; at < ranks; ++at)
                                        sends[at].age = -3;
                                } else {
                                    sends[1].age = -4;
                                }
                            }
                            auto got_op = group.all_to_all_object(
                                std::move(sends), EdgeCodec{});
                            auto got = co_await got_op;
                            if (!got) outcome = std::move(got).error();
                        }
                        co_return outcome;
                    });
                for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                    REQUIRE(errors[rank].has_value());
                    // A rank keeps its own piece without serializing it, so
                    // with two ranks the one piece it sends cannot overflow the
                    // sum and fails as a block too large.
                    const bool one_piece = which == "huge sum" && ranks == 2;
                    CHECK(has_text(*errors[rank], one_piece
                                                      ? "do not fit one block"
                                                      : one.text));
                }
            }
        }
    }

    TEST_CASE(
        "ranks that pass other types to an object call fail on every rank") {
        for (const int which : {0, 1}) {
            for (const std::uint32_t ranks : {2u, 3u}) {
                CAPTURE(which);
                CAPTURE(ranks);
                Runtime rt(4);
                comm::Communicator communicator =
                    start(ranks, *rt.executor(), Binding::Local);
                auto errors =
                    run_ranks(rt, ranks, [&](std::uint32_t rank) -> Task {
                        comm::Group group = communicator.world(rank);
                        dstd::Result<void> outcome;
                        if (rank != 0) {
                            auto got_op = group.all_gather_object(
                                Person{"x", 1}, PersonCodec{});
                            auto got = co_await got_op;
                            if (!got) outcome = std::move(got).error();
                        } else if (which == 0) {
                            auto got_op = group.all_gather_object(
                                Number{1}, NumberCodec{});
                            auto got = co_await got_op;
                            if (!got) outcome = std::move(got).error();
                        } else if (which == 1) {
                            const dstd::Vector<std::int32_t> one = {1};
                            auto got_op = group.all_gather(
                                dstd::Span<const std::int32_t>(one));
                            auto got = co_await got_op;
                            if (!got) outcome = std::move(got).error();
                        }
                        co_return outcome;
                    });
                for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                    REQUIRE(errors[rank].has_value());
                    CHECK(has_text(*errors[rank], "element size"));
                }
            }
        }
    }

    TEST_CASE("a process that holds one rank ends only the call it holds") {
        Runtime rt(2);
        SUBCASE("personal table") {
            comm::Communicator communicator =
                start_one_rank(*rt.executor(),
                               {bytes_of_values<std::int32_t>({1}),
                                bytes_of_values<std::int32_t>({2})},
                               true);
            comm::Group group = communicator.world(0);
            const dstd::Vector<std::int32_t> values = {7, 8};
            auto got =
                group.all_to_all(dstd::Span<const std::int32_t>(values)).wait();
            REQUIRE(got.has_value());
            REQUIRE(got->size() == 2);
            CHECK(got->as<std::int32_t>(1)[0] == 2);
        }
        SUBCASE("shared table") {
            comm::Communicator communicator =
                start_one_rank(*rt.executor(),
                               {bytes_of_values<std::int32_t>({1}),
                                bytes_of_values<std::int32_t>({2})},
                               false);
            comm::Group group = communicator.world(1);
            const dstd::Vector<std::int32_t> values = {7};
            auto got =
                group.all_gather(dstd::Span<const std::int32_t>(values)).wait();
            REQUIRE(got.has_value());
            CHECK(got->as<std::int32_t>(0)[0] == 1);
        }
    }

    TEST_CASE("a table entry of the wrong size is an error, not a value") {
        Runtime rt(2);
        const dstd::Vector<std::int32_t> two = {1, 2};
        SUBCASE("broadcast of values") {
            comm::Communicator communicator = start_one_rank(
                *rt.executor(),
                {dstd::Vector<std::byte>(5), dstd::Vector<std::byte>(5)},
                false);
            auto got = communicator.world(0)
                           .broadcast(0, dstd::Span<const std::int32_t>(two))
                           .wait();
            REQUIRE_FALSE(got.has_value());
            CHECK(has_text(got.error(), "whole number"));
        }
        SUBCASE("broadcast of one value") {
            comm::Communicator communicator = start_one_rank(
                *rt.executor(),
                {dstd::Vector<std::byte>(3), dstd::Vector<std::byte>(3)},
                false);
            auto got =
                communicator.world(0).broadcast(0, std::int32_t{1}).wait();
            REQUIRE_FALSE(got.has_value());
            CHECK(has_text(got.error(), "not one"));
        }
        SUBCASE("scan entry of another rank") {
            comm::Communicator communicator =
                start_one_rank(*rt.executor(),
                               {bytes_of_values<std::int32_t>({1, 2}),
                                bytes_of_values<std::int32_t>({1})},
                               false);
            auto got = communicator.world(1)
                           .scan(dstd::Span<const std::int32_t>(two),
                                 comm::ReduceOp::Sum)
                           .wait();
            REQUIRE_FALSE(got.has_value());
            CHECK(has_text(got.error(), "rank 1 gave 4 bytes"));
        }
        SUBCASE("scan entry of rank 0 against the count") {
            comm::Communicator communicator =
                start_one_rank(*rt.executor(),
                               {bytes_of_values<std::int32_t>({1}),
                                bytes_of_values<std::int32_t>({1, 2})},
                               false);
            auto got = communicator.world(0)
                           .scan(dstd::Span<const std::int32_t>(two),
                                 comm::ReduceOp::Sum)
                           .wait();
            REQUIRE_FALSE(got.has_value());
            CHECK(has_text(got.error(), "rank 0 gave 4 bytes"));
        }
    }

    TEST_CASE("a deadline names who did not call the new collectives") {
        for (const Binding binding : BINDINGS) {
            for (const int which : {0, 1, 2, 3, 4}) {
                CAPTURE(binding_name(binding));
                CAPTURE(which);
                Runtime rt(4);
                comm::Communicator communicator =
                    start(3, *rt.executor(), binding);
                auto errors = run_ranks(rt, 3, [&](std::uint32_t rank) -> Task {
                    if (rank == 2) co_return {};
                    comm::Group group = communicator.world(rank);
                    comm::CallOpts opts;
                    opts.deadline_ns = dftu_comm_test::deadline_after(
                        rank == 0 ? 40'000'000 : 400'000'000);
                    const dstd::Vector<std::int32_t> values = {1, 2, 3};
                    const dstd::Vector<std::uint32_t> out = {1};
                    dstd::Result<void> outcome;
                    if (which == 0) {
                        auto got_op = group.all_to_all(
                            dstd::Span<const std::int32_t>(values), opts);
                        auto got = co_await got_op;
                        if (!got) outcome = std::move(got).error();
                    } else if (which == 1) {
                        auto got_op = group.reduce_scatter(
                            dstd::Span<const std::int32_t>(values),
                            comm::ReduceOp::Sum, opts);
                        auto got = co_await got_op;
                        if (!got) outcome = std::move(got).error();
                    } else if (which == 2) {
                        auto got_op =
                            group.scan(dstd::Span<const std::int32_t>(values),
                                       comm::ReduceOp::Sum, opts);
                        auto got = co_await got_op;
                        if (!got) outcome = std::move(got).error();
                    } else if (which == 3) {
                        auto got_op = group.exclusive_scan(
                            dstd::Span<const std::int32_t>(values),
                            comm::ReduceOp::Sum, opts);
                        auto got = co_await got_op;
                        if (!got) outcome = std::move(got).error();
                    } else {
                        auto got_op = group.neighbors(
                            dstd::Span<const std::uint32_t>(out), opts);
                        auto got = co_await got_op;
                        if (!got) outcome = std::move(got).error();
                    }
                    co_return outcome;
                });
                REQUIRE(errors[0].has_value());
                REQUIRE(errors[1].has_value());
                CHECK(errors[0]->is(dstd::Condition::Timeout));
                CHECK(has_text(*errors[0], "missing ranks: 2"));
                CHECK(has_text(*errors[1], "aborted by rank 0"));
            }
        }
    }

    TEST_CASE("a cancel aborts the group of the new collectives") {
        for (const Binding binding : BINDINGS) {
            for (const int which : {0, 1}) {
                CAPTURE(binding_name(binding));
                CAPTURE(which);
                Runtime rt(4);
                comm::Communicator communicator =
                    start(2, *rt.executor(), binding);
                std::atomic<bool> cancelled{false};
                auto errors = run_ranks(rt, 2, [&](std::uint32_t rank) -> Task {
                    comm::Group group = communicator.world(rank);
                    const dstd::Vector<std::int32_t> values = {1, 2};
                    if (rank == 1)
                        co_await dftu_comm_test::wait_flag(cancelled);
                    dstd::Result<void> outcome;
                    if (which == 0) {
                        comm::Op<comm::Gathered> op = group.all_to_all(
                            dstd::Span<const std::int32_t>(values));
                        if (rank == 0) {
                            op.cancel();
                            cancelled.store(true, std::memory_order_release);
                        }
                        auto got = co_await op;
                        if (!got) outcome = std::move(got).error();
                    } else {
                        comm::Op<dstd::Vector<std::int32_t>> op =
                            group.scan(dstd::Span<const std::int32_t>(values),
                                       comm::ReduceOp::Sum);
                        if (rank == 0) {
                            op.cancel();
                            cancelled.store(true, std::memory_order_release);
                        }
                        auto got = co_await op;
                        if (!got) outcome = std::move(got).error();
                    }
                    co_return outcome;
                });
                REQUIRE(errors[0].has_value());
                REQUIRE(errors[1].has_value());
                CHECK(errors[0]->is(dstd::Condition::Cancelled));
                CHECK(errors[1]->is(dstd::Condition::Cancelled));
                CHECK(has_text(*errors[1], "aborted by rank 0"));
            }
        }
    }

    TEST_CASE(
        "a provider with an instance per rank serves each its own blocks") {
        for (const std::uint32_t ranks : {2u, 3u, 4u}) {
            CAPTURE(ranks);
            const std::uint32_t driver = ranks - 1;
            Runtime rt(4);
            comm::Communicator communicator =
                start_per_rank_all(ranks, *rt.executor(), driver);
            dstd::Vector<comm::Gathered> swapped(ranks);
            dstd::Vector<comm::Gathered> rows(ranks);
            dstd::Vector<std::uint32_t> from_neighbor(ranks);
            dstd::Vector<dstd::Vector<std::int64_t>> prefix(ranks);
            dstd::Vector<dstd::Vector<std::int64_t>> scattered(ranks);
            auto errors = run_ranks(rt, ranks, [&](std::uint32_t rank) -> Task {
                comm::Group group = communicator.world(rank);
                dstd::Vector<std::string> texts;
                for (std::uint32_t dest = 0; dest < ranks; ++dest)
                    texts.push_back(piece_of(rank, dest));
                dstd::Vector<dstd::Span<const std::byte>> blocks;
                for (const std::string& text : texts)
                    blocks.push_back(bytes_of(text));
                auto a_op = group.all_to_all(
                    dstd::Span<const dstd::Span<const std::byte>>(
                        blocks.data(), blocks.size()));
                auto a = co_await a_op;
                if (!a) co_return std::move(a).error();
                swapped[rank] = *a;
                const dstd::Vector<Row> mine = {
                    Row{0, 0, rank}, Row{1, 1, rank}, Row{2, 2, rank}};
                const std::uint32_t count = ranks;
                auto b_op = group.exchange_by(
                    dstd::Span<const Row>(mine),
                    [count](const Row& row) { return row.key % count; });
                auto b = co_await b_op;
                if (!b) co_return std::move(b).error();
                rows[rank] = *b;
                const dstd::Vector<std::uint32_t> out = {(rank + 1) % ranks};
                auto c_op =
                    group.neighbors(dstd::Span<const std::uint32_t>(out));
                auto c = co_await c_op;
                if (!c) co_return std::move(c).error();
                comm::Neighborhood hood = *c;
                const dstd::Vector<std::uint32_t> numbers = {rank};
                auto d_op = hood.neighbor_exchange(
                    dstd::Span<const std::uint32_t>(numbers));
                auto d = co_await d_op;
                if (!d) co_return std::move(d).error();
                from_neighbor[rank] = d->as<std::uint32_t>(0)[0];
                const dstd::Vector<std::int64_t> values(ranks, rank + 1);
                auto e_op = group.scan(dstd::Span<const std::int64_t>(values),
                                       comm::ReduceOp::Sum);
                auto e = co_await e_op;
                if (!e) co_return std::move(e).error();
                prefix[rank] = *e;
                auto f_op =
                    group.reduce_scatter(dstd::Span<const std::int64_t>(values),
                                         comm::ReduceOp::Sum);
                auto f = co_await f_op;
                if (!f) co_return std::move(f).error();
                scattered[rank] = *f;
                co_return {};
            });
            CHECK(all_ok(errors));
            for (std::uint32_t rank = 0; rank < ranks; ++rank) {
                REQUIRE(swapped[rank].size() == ranks);
                for (std::uint32_t from = 0; from < ranks; ++from)
                    CHECK(text_of(swapped[rank][from]) == piece_of(from, rank));
                std::size_t rows_in = 0;
                std::size_t rows_expected = 0;
                for (std::uint32_t key = 0; key < 3; ++key)
                    if (key % ranks == rank) rows_expected += ranks;
                for (std::uint32_t from = 0; from < ranks; ++from) {
                    for (const Row& row : rows[rank].as<Row>(from)) {
                        CHECK(row.from == from);
                        CHECK(row.key % ranks == rank);
                        ++rows_in;
                    }
                }
                CHECK(rows_in == rows_expected);
                CHECK(from_neighbor[rank] == (rank + ranks - 1) % ranks);
                REQUIRE(prefix[rank].size() == ranks);
                CHECK(prefix[rank][0] ==
                      std::int64_t{rank + 1} * (rank + 2) / 2);
                REQUIRE(scattered[rank].size() == 1);
                CHECK(scattered[rank][0] ==
                      std::int64_t{ranks} * (ranks + 1) / 2);
            }
        }
    }

    TEST_CASE("a provider gives each member only the pieces addressed to it") {
        auto pack = [](const dstd::Vector<std::string>& pieces) {
            auto block = std::make_shared<dstd::Vector<std::byte>>();
            for (const std::string& piece : pieces) {
                const std::uint64_t size = piece.size();
                const auto raw =
                    std::as_bytes(dstd::Span<const std::uint64_t>(&size, 1));
                block->insert(block->end(), raw.begin(), raw.end());
            }
            for (const std::string& piece : pieces) {
                const auto raw = bytes_of(piece);
                block->insert(block->end(), raw.begin(), raw.end());
            }
            return comm::backend::Contribution{
                dstd::Span<const std::byte>(block->data(), block->size()),
                block,
                {}};
        };
        comm::backend::Record record;
        record.flags = comm::backend::RECORD_TABLE |
                       comm::backend::RECORD_WANTS |
                       comm::backend::RECORD_PERSONAL;
        const dstd::Vector<dstd::Vector<std::string>> sends = {
            {"a0", "a1", "a2"}, {"b0", "", "b2b"}, {"c0", "c1", "c2"}};
        for (const bool linear : {false, true}) {
            CAPTURE(linear);
            auto messaging = comm::backend::make_local_messaging();
            dstd::Vector<dstd::SharedPtr<comm::backend::Collectives>> members;
            auto shared = comm::backend::make_local_collectives();
            for (std::uint32_t rank = 0; rank < 3; ++rank)
                members.push_back(
                    linear ? comm::backend::make_linear_collectives(messaging)
                           : shared);
            dstd::Vector<comm::backend::Table> per_dest(3);
            dstd::Vector<std::size_t> served;
            for (std::uint32_t rank = 0; rank < 3; ++rank) {
                members[rank]->arrive(
                    1, 0, 3, rank, record, pack(sends[rank]),
                    [&](const comm::backend::SlotEnd& end) {
                        REQUIRE(end.why == nullptr);
                        REQUIRE(end.tables.size() == 3);
                        std::size_t count = 0;
                        for (std::uint32_t dest = 0; dest < 3; ++dest) {
                            if (end.tables[dest].offsets.empty()) continue;
                            per_dest[dest] = end.tables[dest];
                            ++count;
                        }
                        served.push_back(count);
                    });
            }
            CHECK(served.size() == (linear ? 3u : 1u));
            std::size_t serving_all = 0;
            for (const std::size_t count : served) {
                CHECK(count >= 1);
                if (count == 3) ++serving_all;
            }
            CHECK(serving_all == 1);
            for (std::uint32_t dest = 0; dest < 3; ++dest) {
                REQUIRE(per_dest[dest].size() == 3);
                std::size_t bytes = 0;
                for (std::uint32_t from = 0; from < 3; ++from) {
                    CHECK(text_of(per_dest[dest].at(from)) ==
                          sends[from][dest]);
                    bytes += sends[from][dest].size();
                }
                CHECK(per_dest[dest].data.size() == bytes);
            }
        }
    }

    TEST_CASE(
        "a personal contribution that does not fit ends the slot in error") {
        comm::backend::Record record;
        record.flags = comm::backend::RECORD_TABLE |
                       comm::backend::RECORD_WANTS |
                       comm::backend::RECORD_PERSONAL;
        auto bytes = std::make_shared<dstd::Vector<std::byte>>(5);
        for (const bool linear : {false, true}) {
            CAPTURE(linear);
            auto messaging = comm::backend::make_local_messaging();
            dstd::SharedPtr<comm::backend::Collectives> provider =
                linear ? comm::backend::make_linear_collectives(messaging)
                       : comm::backend::make_local_collectives();
            std::string failure;
            provider->arrive(
                1, 0, 1, 0, record,
                comm::backend::Contribution{
                    dstd::Span<const std::byte>(bytes->data(), bytes->size()),
                    bytes,
                    {}},
                [&](const comm::backend::SlotEnd& end) {
                    if (end.why != nullptr)
                        failure = std::string(end.why->message());
                });
            CHECK(failure.find("one piece per member") != std::string::npos);
        }
    }
}
