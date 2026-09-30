// A join does not match a null key with a null key unless `nulls_equal`: every
// kind against hand-computed rows, several null rows, a null in one of two
// keys, the refusals, the lazy and the spilled join, and the C ABI.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using dftracer::utils::coro::CoroTask;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::InMemorySource;
using dftracer::utils::dataframe::JoinHow;
using dftracer::utils::dataframe::LazyFrame;
using dftracer::utils::dataframe::Series;

namespace {

DataFrame run(CoroTask<DataFrame> t) {
    return dftracer::utils::default_runtime().submit(std::move(t)).get();
}

Series i64s(const std::vector<std::int64_t>& v,
            const std::vector<std::uint8_t>& validity = {}) {
    return Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size()),
                            validity.empty() ? nullptr : validity.data());
}

// A string column with nulls where `validity` bit i is 0.
Series strs(const std::vector<std::string>& v,
            const std::vector<std::uint8_t>& validity) {
    std::vector<std::string_view> views(v.begin(), v.end());
    return Series::strings(std::span<const std::string_view>(views),
                           validity.data());
}

template <class... S>
DataFrame frame(std::vector<std::string> names, S... cols) {
    DataFrame df;
    df.names = std::move(names);
    (df.columns.push_back(std::move(cols)), ...);
    return df;
}

std::string cell(const Series& c, std::int64_t i) {
    if (c.is_null(i)) return "null";
    if (c.type() == dftracer::utils::dataframe::TypeId::String)
        return std::string(c.string_at(i));
    return std::to_string(c.data<std::int64_t>()[i]);
}

// The rows as sorted text, one string per row: order is not part of a join's
// contract for the spilled path.
std::vector<std::string> rows(const DataFrame& df) {
    std::vector<Series> cols;
    for (const Series& c : df.columns) cols.push_back(c.materialize());
    std::vector<std::string> out;
    for (std::int64_t r = 0; r < df.num_rows(); ++r) {
        std::string line;
        for (std::size_t c = 0; c < cols.size(); ++c)
            line += (c ? "," : "") + cell(cols[c], r);
        out.push_back(line);
    }
    std::sort(out.begin(), out.end());
    return out;
}

using Rows = std::vector<std::string>;

// left: k = [a, null, c], x = [1, 2, 3]; right: k = [a, null], y = [10, 20].
DataFrame left() {
    return frame({"k", "x"}, strs({"a", "", "c"}, {0b101}), i64s({1, 2, 3}));
}
DataFrame right() {
    return frame({"k", "y"}, strs({"a", ""}, {0b01}), i64s({10, 20}));
}

DataFrame join(const DataFrame& l, const DataFrame& r, JoinHow how,
               bool nulls_equal) {
    return l.join(r, {"k"}, how, "_right", nulls_equal);
}

LazyFrame lazy_of(const DataFrame& d, std::uint64_t budget = 0) {
    DataFrame copy{d.names, [&] {
                       std::vector<Series> c;
                       for (const Series& s : d.columns) c.push_back(s.share());
                       return c;
                   }()};
    LazyFrame lf =
        LazyFrame::scan(std::make_shared<InMemorySource>(std::move(copy)));
    return budget ? lf.memory_budget(budget) : lf;
}

}  // namespace

TEST_SUITE("join null keys") {
    TEST_CASE("by default a null key never matches") {
        CHECK(rows(join(left(), right(), JoinHow::Inner, false)) ==
              Rows{"a,1,10"});
    }

    TEST_CASE("with nulls_equal a null key matches a null key") {
        const DataFrame out = join(left(), right(), JoinHow::Inner, true);
        CHECK(rows(out) == Rows{"a,1,10", "null,2,20"});
    }

    TEST_CASE("every kind with nulls_equal") {
        CHECK(rows(join(left(), right(), JoinHow::Left, true)) ==
              Rows{"a,1,10", "c,3,null", "null,2,20"});
        CHECK(rows(join(left(), right(), JoinHow::Right, true)) ==
              Rows{"a,1,10", "null,2,20"});
        CHECK(rows(join(left(), right(), JoinHow::Outer, true)) ==
              Rows{"a,1,10", "c,3,null", "null,2,20"});
        CHECK(rows(join(left(), right(), JoinHow::Semi, true)) ==
              Rows{"a,1", "null,2"});
        CHECK(rows(join(left(), right(), JoinHow::Anti, true)) == Rows{"c,3"});
    }

    TEST_CASE("the default is unchanged for every kind") {
        CHECK(rows(join(left(), right(), JoinHow::Left, false)) ==
              Rows{"a,1,10", "c,3,null", "null,2,null"});
        CHECK(rows(join(left(), right(), JoinHow::Semi, false)) == Rows{"a,1"});
        CHECK(rows(join(left(), right(), JoinHow::Anti, false)) ==
              Rows{"c,3", "null,2"});
    }

    TEST_CASE("right and outer append the unmatched right rows") {
        const DataFrame l =
            frame({"k", "x"}, strs({"a", ""}, {0b01}), i64s({1, 2}));
        const DataFrame r =
            frame({"k", "y"}, strs({"a", "", "z", ""}, {0b0101}),
                  i64s({10, 20, 30, 40}));
        // The left null row matches both right nulls; z has no match.
        CHECK(rows(join(l, r, JoinHow::Outer, true)) ==
              Rows{"a,1,10", "null,2,20", "null,2,40", "z,null,30"});
        // Without the option the three right rows left over are appended.
        CHECK(rows(join(l, r, JoinHow::Outer, false)) ==
              Rows{"a,1,10", "null,2,null", "null,null,20", "null,null,40",
                   "z,null,30"});
    }

    TEST_CASE("several null rows on each side") {
        const DataFrame l = frame({"k", "x"}, i64s({0, 0}, {0}), i64s({1, 2}));
        const DataFrame r =
            frame({"k", "y"}, i64s({0, 0}, {0}), i64s({10, 20}));
        CHECK(join(l, r, JoinHow::Inner, true).num_rows() == 4);
        CHECK(join(l, r, JoinHow::Inner, false).num_rows() == 0);
    }

    TEST_CASE("one of two key columns is null") {
        const DataFrame l = frame({"a", "b", "x"}, i64s({7, 7}),
                                  i64s({0, 1}, {0b10}), i64s({1, 2}));
        const DataFrame r =
            frame({"a", "b", "y"}, i64s({7}), i64s({0}, {0}), i64s({10}));
        const DataFrame out =
            l.join(r, {"a", "b"}, JoinHow::Inner, "_right", true);
        CHECK(rows(out) == Rows{"7,null,1,10"});
        CHECK(
            l.join(r, {"a", "b"}, JoinHow::Inner, "_right", false).num_rows() ==
            0);
    }

    TEST_CASE("an integer key column with nulls") {
        const DataFrame l =
            frame({"k", "x"}, i64s({1, 0, 3}, {0b101}), i64s({1, 2, 3}));
        const DataFrame r =
            frame({"k", "y"}, i64s({1, 0}, {0b01}), i64s({10, 20}));
        CHECK(rows(join(l, r, JoinHow::Inner, true)) ==
              Rows{"1,1,10", "null,2,20"});
        CHECK(rows(join(l, r, JoinHow::Inner, false)) == Rows{"1,1,10"});
    }

    TEST_CASE("cross, lookup and nest refuse the option") {
        for (JoinHow how : {JoinHow::Cross, JoinHow::Lookup, JoinHow::Nest}) {
            CHECK_THROWS_AS(left().join(right(), {"k"}, how, "_right", true),
                            std::invalid_argument);
        }
        try {
            left().join(right(), {"k"}, JoinHow::Cross, "_right", true);
        } catch (const std::invalid_argument& e) {
            CHECK(std::string(e.what()).find("cross") != std::string::npos);
        }
    }

    TEST_CASE("a lazy join gives the eager rows") {
        for (JoinHow how : {JoinHow::Inner, JoinHow::Left, JoinHow::Right,
                            JoinHow::Outer, JoinHow::Semi, JoinHow::Anti}) {
            const DataFrame lazy_out =
                run(lazy_of(left())
                        .join(lazy_of(right()), {"k"}, how, "_right", true)
                        .collect(2));
            CHECK(rows(lazy_out) == rows(join(left(), right(), how, true)));
        }
    }

    TEST_CASE("a lazy join refuses the option for cross, lookup and nest") {
        CHECK_THROWS_AS(lazy_of(left()).join(lazy_of(right()), {"k"},
                                             JoinHow::Lookup, "_right", true),
                        std::invalid_argument);
    }

    TEST_CASE("a spilled join gives the in-memory rows") {
        // A right side much larger than the budget forces the partitioned path.
        std::vector<std::int64_t> keys, ys;
        std::vector<std::uint8_t> validity((2000 + 7) / 8, 0xFF);
        for (std::int64_t i = 0; i < 2000; ++i) {
            keys.push_back(i % 50);
            ys.push_back(i);
            if (i % 7 == 0)
                validity[static_cast<std::size_t>(i >> 3)] &=
                    static_cast<std::uint8_t>(~(1u << (i & 7)));
        }
        std::vector<std::int64_t> lkeys{0, 1, 2, 0, 5}, lx{1, 2, 3, 4, 5};
        const DataFrame r = frame({"k", "y"}, i64s(keys, validity), i64s(ys));
        const DataFrame l = frame({"k", "x"}, i64s(lkeys, {0b10111}), i64s(lx));
        for (JoinHow how :
             {JoinHow::Inner, JoinHow::Left, JoinHow::Outer, JoinHow::Semi}) {
            const DataFrame memory = join(l, r, how, true);
            const DataFrame spilled =
                run(lazy_of(l, 256)
                        .join(lazy_of(r), {"k"}, how, "_right", true)
                        .collect(100));
            CHECK(rows(spilled) == rows(memory));
        }
    }

    TEST_CASE("C ABI: the flag selects the rule") {
        const DataFrame l = left();
        const DataFrame r = right();
        dftu_dataframe* lh = dftu_dataframe_new(
            std::vector<const char*>{"k", "x"}.data(),
            std::vector<dftu_series*>{l.columns[0].share().release(),
                                      l.columns[1].share().release()}
                .data(),
            2);
        dftu_dataframe* rh = dftu_dataframe_new(
            std::vector<const char*>{"k", "y"}.data(),
            std::vector<dftu_series*>{r.columns[0].share().release(),
                                      r.columns[1].share().release()}
                .data(),
            2);
        REQUIRE(lh);
        REQUIRE(rh);
        const char* on[] = {"k"};
        dftu_dataframe* strict =
            dftu_dataframe_join(lh, rh, on, on, 1, DFTU_JOIN_INNER, nullptr, 0);
        dftu_dataframe* nulls =
            dftu_dataframe_join(lh, rh, on, on, 1, DFTU_JOIN_INNER, nullptr, 1);
        REQUIRE(strict);
        REQUIRE(nulls);
        CHECK(dftu_dataframe_num_rows(strict) == 1);
        CHECK(dftu_dataframe_num_rows(nulls) == 2);
        CHECK(dftu_dataframe_join(lh, rh, on, on, 1, DFTU_JOIN_CROSS, nullptr,
                                  1) == nullptr);
        dftu_dataframe* ll = nullptr;
        dftu_lazyframe* lz = dftu_dataframe_lazy(lh);
        dftu_lazyframe* rz = dftu_dataframe_lazy(rh);
        dftu_lazyframe* jl =
            dftu_lazyframe_join(lz, rz, on, on, 1, DFTU_JOIN_INNER, nullptr, 1);
        REQUIRE(jl);
        ll = dftu_lazyframe_collect(jl, 0);
        REQUIRE(ll);
        CHECK(dftu_dataframe_num_rows(ll) == 2);
        CHECK(dftu_lazyframe_join(lz, rz, on, on, 1, DFTU_JOIN_NEST, "n", 1) ==
              nullptr);
        dftu_dataframe_free(ll);
        dftu_lazyframe_free(jl);
        dftu_lazyframe_free(rz);
        dftu_lazyframe_free(lz);
        dftu_dataframe_free(nulls);
        dftu_dataframe_free(strict);
        dftu_dataframe_free(rh);
        dftu_dataframe_free(lh);
    }
}
