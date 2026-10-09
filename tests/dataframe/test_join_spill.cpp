// A join whose right side outgrows the memory budget runs partitioned on disk
// and gives the rows of the in-memory join, checked against a nested loop.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/grace_join.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using dftracer::utils::NO_SPILL_BUDGET;
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

struct Rng {
    std::uint64_t x;
    std::uint64_t next(std::uint64_t m) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        return (x >> 33) % m;
    }
};

using Cell = std::optional<std::int64_t>;

struct Table {
    std::vector<Cell> key;
    std::vector<std::int64_t> value;
};

// Ten percent null keys, and one key that holds `skew` percent of the rows.
Table random_table(Rng& rng, int n, int keys, int skew, std::int64_t scale) {
    Table t;
    for (int i = 0; i < n; ++i) {
        if (rng.next(10) == 0)
            t.key.push_back(std::nullopt);
        else if (static_cast<int>(rng.next(100)) < skew)
            t.key.push_back(3);
        else
            t.key.push_back(static_cast<std::int64_t>(rng.next(keys)));
        t.value.push_back(i * scale);
    }
    return t;
}

DataFrame frame_of(const Table& t, const char* key, const char* value) {
    std::vector<std::int64_t> k;
    std::vector<std::uint8_t> bits((t.key.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < t.key.size(); ++i) {
        k.push_back(t.key[i].value_or(0));
        if (t.key[i]) bits[i >> 3] |= static_cast<std::uint8_t>(1U << (i & 7));
    }
    DataFrame f;
    f.names = {key, value};
    f.columns.push_back(Series::flat_i64(
        k.data(), static_cast<std::int64_t>(k.size()), bits.data()));
    f.columns.push_back(Series::flat_i64(
        t.value.data(), static_cast<std::int64_t>(t.value.size())));
    return f;
}

std::string text(const Cell& c) { return c ? std::to_string(*c) : "n"; }

std::vector<std::string> rows_of(const DataFrame& f) {
    std::vector<std::string> out;
    std::vector<Series> cols;
    for (const Series& c : f.columns) cols.push_back(c.materialize());
    for (std::int64_t r = 0; r < f.num_rows(); ++r) {
        std::string row;
        for (const Series& c : cols)
            row += (c.is_null(r) ? std::string("n")
                                 : std::to_string(c.data<std::int64_t>()[r])) +
                   ",";
        out.push_back(row);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The nested-loop join of left(lk, lv) and right(rk, rv) on lk == rk.
std::vector<std::string> reference(const Table& l, const Table& r,
                                   JoinHow how) {
    std::vector<std::string> out;
    std::vector<char> matched(r.key.size(), 0);
    for (std::size_t i = 0; i < l.key.size(); ++i) {
        bool any = false;
        for (std::size_t j = 0; j < r.key.size(); ++j) {
            if (!l.key[i] || !r.key[j] || *l.key[i] != *r.key[j]) continue;
            any = true;
            matched[j] = 1;
            if (how == JoinHow::Semi || how == JoinHow::Anti) break;
            out.push_back(text(l.key[i]) + "," + std::to_string(l.value[i]) +
                          "," + text(r.key[j]) + "," +
                          std::to_string(r.value[j]) + ",");
        }
        const std::string left =
            text(l.key[i]) + "," + std::to_string(l.value[i]) + ",";
        if (how == JoinHow::Semi && any) out.push_back(left);
        if (how == JoinHow::Anti && !any) out.push_back(left);
        if (!any && (how == JoinHow::Left || how == JoinHow::Outer))
            out.push_back(left + "n,n,");
    }
    if (how == JoinHow::Right || how == JoinHow::Outer)
        for (std::size_t j = 0; j < r.key.size(); ++j)
            if (!matched[j])
                out.push_back("n,n," + text(r.key[j]) + "," +
                              std::to_string(r.value[j]) + ",");
    std::sort(out.begin(), out.end());
    return out;
}

DataFrame join(const DataFrame& l, const DataFrame& r, JoinHow how,
               std::uint64_t budget, const std::string& suffix = "") {
    const LazyFrame left =
        LazyFrame::scan(std::make_shared<InMemorySource>(
                            DataFrame{l.names,
                                      [&] {
                                          std::vector<Series> c;
                                          for (const auto& s : l.columns)
                                              c.push_back(s.share());
                                          return c;
                                      }()}))
            .memory_budget(budget);
    const LazyFrame right = LazyFrame::scan(std::make_shared<InMemorySource>(
        DataFrame{r.names, [&] {
                      std::vector<Series> c;
                      for (const auto& s : r.columns) c.push_back(s.share());
                      return c;
                  }()}));
    return run(left.join(right, {"lk"}, {"rk"}, how, suffix).collect(200));
}

// One row per left row: its two columns and the list of (rk, rv) cells, in
// the list's order; the rows sorted.
std::vector<std::string> nest_rows_of(const DataFrame& f) {
    const Series lk = f.column("lk").materialize();
    const Series lv = f.column("lv").materialize();
    const Series m = f.column("m").materialize();
    const Series rows = m.child(0);
    const Series rk = rows.child(0).materialize();
    const Series rv = rows.child(1).materialize();
    std::vector<std::string> out;
    for (std::int64_t i = 0; i < f.num_rows(); ++i) {
        std::string row =
            (lk.is_null(i) ? std::string("n")
                           : std::to_string(lk.data<std::int64_t>()[i])) +
            "," + std::to_string(lv.data<std::int64_t>()[i]) + ":";
        for (std::int32_t j = m.offsets()[i]; j < m.offsets()[i + 1]; ++j)
            row += std::to_string(rk.data<std::int64_t>()[j]) + "=" +
                   std::to_string(rv.data<std::int64_t>()[j]) + ";";
        out.push_back(row);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Left rows over null keys, key 3 and a few others; right rows in which the
// value is a function of the key (a lookup needs that), key 3 holding most.
void block_tables(Table& l, Table& r) {
    for (int i = 0; i < 400; ++i) {
        l.key.push_back(i % 7 == 0   ? Cell()
                        : i % 3 == 0 ? Cell(i % 11)
                                     : Cell(3));
        l.value.push_back(i);
    }
    for (int i = 0; i < 1500; ++i) {
        r.key.push_back(i % 13 == 0  ? Cell()
                        : i % 9 == 0 ? Cell(i % 17)
                                     : Cell(3));
        r.value.push_back(r.key.back() ? *r.key.back() * 10 : i);
    }
}

std::string error_of(const DataFrame& l, const DataFrame& r, JoinHow how,
                     std::uint64_t budget) {
    try {
        (void)join(l, r, how, budget);
    } catch (const std::invalid_argument& e) {
        return e.what();
    }
    return "";
}

const JoinHow HOWS[] = {JoinHow::Inner, JoinHow::Left, JoinHow::Right,
                        JoinHow::Outer, JoinHow::Semi, JoinHow::Anti};

}  // namespace

TEST_SUITE("join spill") {
    TEST_CASE("a spilled join equals the nested loop and the in-memory join") {
        Rng rng{11};
        const Table l = random_table(rng, 2000, 60, 8, 1);
        const Table r = random_table(rng, 1500, 60, 8, 10);
        const DataFrame lf = frame_of(l, "lk", "lv");
        const DataFrame rf = frame_of(r, "rk", "rv");
        for (const JoinHow how : HOWS) {
            CAPTURE(static_cast<int>(how));
            const auto want = reference(l, r, how);
            CHECK(rows_of(join(lf, rf, how, NO_SPILL_BUDGET)) == want);
            CHECK(rows_of(join(lf, rf, how, 4096)) == want);
            CHECK(rows_of(join(lf, rf, how, 1)) == want);
        }
    }

    TEST_CASE("one key that fills the right side splits to the depth limit") {
        Rng rng{5};
        Table r;
        Table l = random_table(rng, 300, 4, 0, 1);
        for (int i = 0; i < 1500; ++i) {
            r.key.push_back(i % 50 == 0 ? Cell(i) : Cell(3));
            r.value.push_back(i);
        }
        const DataFrame lf = frame_of(l, "lk", "lv");
        const DataFrame rf = frame_of(r, "rk", "rv");
        for (const JoinHow how : {JoinHow::Inner, JoinHow::Outer})
            CHECK(rows_of(join(lf, rf, how, 512)) == reference(l, r, how));
    }

    TEST_CASE(
        "one key that most rows of both sides hold joins block by block") {
        Table l, r;
        for (int i = 0; i < 400; ++i) {
            l.key.push_back(i % 7 == 0   ? Cell()
                            : i % 3 == 0 ? Cell(i % 11)
                                         : Cell(3));
            l.value.push_back(i);
        }
        for (int i = 0; i < 1500; ++i) {
            r.key.push_back(i % 13 == 0  ? Cell()
                            : i % 9 == 0 ? Cell(i % 17)
                                         : Cell(3));
            r.value.push_back(i * 10);
        }
        const DataFrame lf = frame_of(l, "lk", "lv");
        const DataFrame rf = frame_of(r, "rk", "rv");
        for (const JoinHow how : HOWS)
            for (const std::uint64_t budget :
                 {std::uint64_t{512}, std::uint64_t{4096}}) {
                CAPTURE(static_cast<int>(how));
                CAPTURE(budget);
                const std::uint64_t before =
                    dftracer::utils::dataframe::grace_join_blocks();
                CHECK(rows_of(join(lf, rf, how, budget)) ==
                      reference(l, r, how));
                CHECK(dftracer::utils::dataframe::grace_join_blocks() >
                      before + 1);
            }
    }

    TEST_CASE("a lookup joins a partition that one key fills block by block") {
        Table l, r;
        block_tables(l, r);
        const DataFrame lf = frame_of(l, "lk", "lv");
        const DataFrame rf = frame_of(r, "rk", "rv");
        const auto want =
            rows_of(join(lf, rf, JoinHow::Lookup, NO_SPILL_BUDGET));
        REQUIRE(want.size() == l.key.size());
        for (const std::uint64_t budget :
             {std::uint64_t{512}, std::uint64_t{4096}}) {
            CAPTURE(budget);
            const std::uint64_t before =
                dftracer::utils::dataframe::grace_join_blocks();
            CHECK(rows_of(join(lf, rf, JoinHow::Lookup, budget)) == want);
            CHECK(dftracer::utils::dataframe::grace_join_blocks() > before + 1);
        }
    }

    TEST_CASE("a lookup block by block refuses the conflict the join refuses") {
        Table l, r;
        block_tables(l, r);
        r.value[1400] += 1;
        r.key[1400] = 3;
        const DataFrame lf = frame_of(l, "lk", "lv");
        const DataFrame rf = frame_of(r, "rk", "rv");
        const std::string want =
            error_of(lf, rf, JoinHow::Lookup, NO_SPILL_BUDGET);
        REQUIRE_FALSE(want.empty());
        for (const std::uint64_t budget :
             {std::uint64_t{512}, std::uint64_t{4096}})
            CHECK(error_of(lf, rf, JoinHow::Lookup, budget) == want);
    }

    TEST_CASE("a nest joins a partition that one key fills block by block") {
        Table l, r;
        block_tables(l, r);
        for (std::size_t i = 0; i < r.value.size(); ++i)
            r.value[i] = static_cast<std::int64_t>(i);
        const DataFrame lf = frame_of(l, "lk", "lv");
        const DataFrame rf = frame_of(r, "rk", "rv");
        const auto want =
            nest_rows_of(join(lf, rf, JoinHow::Nest, NO_SPILL_BUDGET, "m"));
        REQUIRE(want.size() == l.key.size());
        for (const std::uint64_t budget :
             {std::uint64_t{512}, std::uint64_t{4096}}) {
            CAPTURE(budget);
            const std::uint64_t before =
                dftracer::utils::dataframe::grace_join_blocks();
            CHECK(nest_rows_of(join(lf, rf, JoinHow::Nest, budget, "m")) ==
                  want);
            CHECK(dftracer::utils::dataframe::grace_join_blocks() > before);
        }
    }

    TEST_CASE("a nest over a partition of null keys gives empty lists") {
        Table l, r;
        for (int i = 0; i < 100; ++i) {
            l.key.push_back(i % 2 ? Cell() : Cell(i));
            l.value.push_back(i);
        }
        for (int i = 0; i < 600; ++i) {
            r.key.push_back(Cell());
            r.value.push_back(i);
        }
        const DataFrame lf = frame_of(l, "lk", "lv");
        const DataFrame rf = frame_of(r, "rk", "rv");
        CHECK(nest_rows_of(join(lf, rf, JoinHow::Nest, 256, "m")) ==
              nest_rows_of(join(lf, rf, JoinHow::Nest, NO_SPILL_BUDGET, "m")));
    }

    TEST_CASE("an empty side and no matching key") {
        Rng rng{2};
        const Table l = random_table(rng, 400, 5, 0, 1);
        Table r;
        for (int i = 0; i < 300; ++i) {
            r.key.push_back(1000 + i);
            r.value.push_back(i);
        }
        const DataFrame lf = frame_of(l, "lk", "lv");
        const DataFrame rf = frame_of(r, "rk", "rv");
        for (const JoinHow how : HOWS)
            CHECK(rows_of(join(lf, rf, how, 256)) == reference(l, r, how));
    }

    TEST_CASE("a lookup matches keys of different number types") {
        std::vector<std::int64_t> lk;
        std::vector<double> rk;
        std::vector<std::int64_t> rv;
        for (int i = 0; i < 600; ++i) lk.push_back(i % 200);
        for (int j = 0; j < 400; ++j) {
            rk.push_back(static_cast<double>(j));
            rv.push_back(j * 7);
        }
        DataFrame l;
        l.names = {"lk"};
        l.columns.push_back(Series::flat_i64(lk.data(), 600));
        DataFrame r;
        r.names = {"rk", "rv"};
        r.columns.push_back(Series::flat_f64(rk.data(), 400));
        r.columns.push_back(Series::flat_i64(rv.data(), 400));
        const auto plain = [&](std::uint64_t budget) {
            return rows_of(join(l, r, JoinHow::Lookup, budget));
        };
        const auto want = plain(NO_SPILL_BUDGET);
        CHECK(want.size() == 600);
        CHECK(plain(300) == want);
    }

    TEST_CASE("a partition without right rows keeps a text column") {
        std::vector<std::int64_t> lk;
        for (int i = 0; i < 1000; ++i) lk.push_back(i);
        std::vector<std::int64_t> rk;
        std::vector<std::string> rv;
        for (int j = 0; j < 40; ++j) {
            rk.push_back(j);
            rv.push_back("v" + std::to_string(j));
        }
        DataFrame l;
        l.names = {"lk"};
        l.columns.push_back(Series::flat_i64(lk.data(), 1000));
        DataFrame r;
        r.names = {"rk", "rv"};
        r.columns.push_back(Series::flat_i64(rk.data(), 40));
        r.columns.push_back(Series::strings(rv));
        const DataFrame out = join(l, r, JoinHow::Lookup, 64);
        REQUIRE(out.num_rows() == 1000);
        const Series k = out.column("lk").materialize();
        const Series v = out.column("rv").materialize();
        for (std::int64_t i = 0; i < out.num_rows(); ++i) {
            const std::int64_t key = k.data<std::int64_t>()[i];
            CHECK(v.is_null(i) == (key >= 40));
            if (key < 40) CHECK(v.string_at(i) == "v" + std::to_string(key));
        }
    }

    TEST_CASE("a list column spills with the join") {
        Rng rng{9};
        const Table l = random_table(rng, 500, 20, 0, 1);
        const Table r = random_table(rng, 500, 20, 0, 10);
        const DataFrame lf = frame_of(l, "lk", "lv");
        DataFrame rf = frame_of(r, "rk", "rv");
        std::vector<std::int32_t> off;
        for (int i = 0; i <= 500; ++i) off.push_back(i);
        rf.names.push_back("tags");
        rf.columns.push_back(Series::list(off, rf.columns[1].share()));
        const DataFrame spilled = join(lf, rf, JoinHow::Inner, 256);
        const DataFrame whole = join(lf, rf, JoinHow::Inner, NO_SPILL_BUDGET);
        CHECK(spilled.num_rows() == whole.num_rows());
        CHECK(spilled.names == whole.names);
        DataFrame plain_spilled, plain_whole;
        for (std::size_t c = 0; c < spilled.names.size(); ++c) {
            if (spilled.names[c] == "tags") continue;
            plain_spilled.names.push_back(spilled.names[c]);
            plain_spilled.columns.push_back(spilled.columns[c].share());
            plain_whole.names.push_back(whole.names[c]);
            plain_whole.columns.push_back(whole.columns[c].share());
        }
        CHECK(rows_of(plain_spilled) == rows_of(plain_whole));
        // The list of a row is the value column's own row: tags[i] == [rv].
        const Series tags = spilled.column("tags").materialize();
        const Series rv = spilled.column("rv").materialize();
        REQUIRE(tags.length() == rv.length());
        for (std::int64_t i = 0; i < tags.length(); ++i)
            CHECK(tags.child(0).data<std::int64_t>()[tags.offsets()[i]] ==
                  rv.data<std::int64_t>()[i]);
    }
}
