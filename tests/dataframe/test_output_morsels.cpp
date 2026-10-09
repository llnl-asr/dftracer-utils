// Ops that make more rows than they read, or one result for the whole input,
// must hand it on in morsels that fit the memory budget, and the rows must be
// the ones an unlimited run gives.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/grace_join.h>
#include <dftracer/utils/dataframe/internal/cell_ops.h>
#include <dftracer/utils/dataframe/internal/native_transform.h>
#include <dftracer/utils/dataframe/lazy/frame_op.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace dftracer::utils::dataframe;
using dftracer::utils::coro::CoroTask;

namespace {

constexpr std::int64_t MAX_ROWS = 1'000'000;

CoroTask<std::vector<DataFrame>> stream_all(LazyFrame lf,
                                            std::int64_t max_rows) {
    std::vector<DataFrame> out;
    auto g = lf.stream(max_rows);
    while (auto df = co_await g.next()) out.push_back(std::move(*df));
    co_return out;
}

std::vector<DataFrame> morsels(const LazyFrame& lf,
                               std::int64_t max_rows = MAX_ROWS) {
    return dftracer::utils::default_runtime()
        .submit(stream_all(lf, max_rows))
        .get();
}

std::int64_t most_rows(const std::vector<DataFrame>& parts) {
    std::int64_t most = 0;
    for (const DataFrame& p : parts) most = std::max(most, p.num_rows());
    return most;
}

void add_keys(const DataFrame& p, std::vector<std::string>& keys) {
    std::vector<Series> cols;
    for (const Series& c : p.columns) cols.push_back(c.materialize());
    for (std::int64_t r = 0; r < p.num_rows(); ++r)
        keys.push_back(row_key(cols, r));
}

// Every row of every morsel as a key, sorted: the rows, whatever their order.
std::vector<std::string> row_set(const std::vector<DataFrame>& parts) {
    std::vector<std::string> keys;
    for (const DataFrame& p : parts) add_keys(p, keys);
    std::sort(keys.begin(), keys.end());
    return keys;
}

std::vector<std::string> row_set(const DataFrame& one) {
    std::vector<std::string> keys;
    add_keys(one, keys);
    std::sort(keys.begin(), keys.end());
    return keys;
}

// The morsels joined in the order they came.
DataFrame joined(const std::vector<DataFrame>& parts) {
    std::vector<const DataFrame*> ptrs;
    for (const DataFrame& p : parts) ptrs.push_back(&p);
    return concat(ptrs, ConcatHow::Vertical);
}

// The same rows in the same order: for the ops that keep the order of their
// input, a mix-up of rows between morsels must not pass.
void expect_same_rows(const DataFrame& got, const DataFrame& want) {
    REQUIRE(got.names == want.names);
    REQUIRE(got.num_rows() == want.num_rows());
    for (std::size_t c = 0; c < got.columns.size(); ++c) {
        std::vector<Series> x, y;
        x.push_back(got.columns[c].materialize());
        y.push_back(want.columns[c].materialize());
        for (std::int64_t r = 0; r < got.num_rows(); ++r)
            REQUIRE(row_key(x, r) == row_key(y, r));
    }
}

DataFrame ints(const std::vector<std::string>& names, std::int64_t rows,
               std::int64_t mod) {
    DataFrame df;
    df.names = names;
    for (std::size_t c = 0; c < names.size(); ++c) {
        std::vector<std::int64_t> v(static_cast<std::size_t>(rows));
        for (std::int64_t i = 0; i < rows; ++i)
            v[static_cast<std::size_t>(i)] =
                (i * static_cast<std::int64_t>(c + 3)) % mod;
        df.columns.push_back(Series::flat_i64(v.data(), rows));
    }
    return df;
}

// `morsels` morsels of `rows` rows each with an ascending time t and v = 1,
// counting how many the plan has pulled.
class CountedTimes : public Source {
   public:
    CountedTimes(std::int64_t morsels, std::int64_t rows,
                 std::shared_ptr<std::int64_t> pulled)
        : morsels_(morsels), rows_(rows), pulled_(std::move(pulled)) {}

    Schema schema() const override {
        const std::int64_t zero = 0;
        const DataType i = Series::flat_i64(&zero, 1).data_type();
        Schema out;
        out.fields = {Field{"t", i, true}, Field{"v", i, true}};
        return out;
    }

    ScanResult scan(const ScanRequest& req) const override {
        ScanResult r;
        r.cursor = std::make_unique<Rows>(morsels_, rows_, pulled_);
        r.filters.assign(req.filters.size(), Pushed::No);
        return r;
    }

   private:
    class Rows : public Cursor {
       public:
        Rows(std::int64_t morsels, std::int64_t rows,
             std::shared_ptr<std::int64_t> pulled)
            : morsels_(morsels), rows_(rows), pulled_(std::move(pulled)) {}
        CoroTask<std::optional<Morsel>> next(std::int64_t) override {
            if (*pulled_ >= morsels_) co_return std::nullopt;
            std::vector<std::int64_t> t(static_cast<std::size_t>(rows_)),
                v(static_cast<std::size_t>(rows_), 1);
            for (std::int64_t i = 0; i < rows_; ++i)
                t[static_cast<std::size_t>(i)] = *pulled_ * rows_ + i;
            ++*pulled_;
            Morsel m;
            m.rows = rows_;
            m.columns.push_back(Series::flat_i64(t.data(), rows_));
            m.columns.push_back(Series::flat_i64(v.data(), rows_));
            co_return m;
        }

       private:
        std::int64_t morsels_, rows_;
        std::shared_ptr<std::int64_t> pulled_;
    };

    std::int64_t morsels_, rows_;
    std::shared_ptr<std::int64_t> pulled_;
};

// The morsels pulled from the source when the plan's first morsel came out.
CoroTask<std::int64_t> pulled_at_first(LazyFrame lf,
                                       std::shared_ptr<std::int64_t> pulled) {
    auto g = lf.stream(1000);
    auto first = co_await g.next();
    REQUIRE(first.has_value());
    co_return *pulled;
}

}  // namespace

TEST_SUITE("output morsels") {
    TEST_CASE("group_by_dynamic hands on a window once the time passes it") {
        // Ascending time: a window behind the newest time is final, so the
        // first windows leave long before the input is read.
        constexpr std::int64_t MORSELS = 50;
        auto pulled = std::make_shared<std::int64_t>(0);
        const LazyFrame plan =
            LazyFrame::scan(
                std::make_shared<CountedTimes>(MORSELS, 1000, pulled))
                .memory_budget(1 << 20)
                .group_by_dynamic("t", 10, 30, {GroupAgg{Agg::Sum, "v", "s"}});
        const std::int64_t at = dftracer::utils::default_runtime()
                                    .submit(pulled_at_first(plan, pulled))
                                    .get();
        CHECK(at < MORSELS / 2);
    }

    TEST_CASE("explode of long lists leaves in morsels of the output share") {
        const std::int64_t rows = 2000, len = 50;
        std::vector<std::int32_t> offs(static_cast<std::size_t>(rows) + 1);
        std::vector<std::int64_t> items(static_cast<std::size_t>(rows * len));
        std::vector<std::int64_t> id(static_cast<std::size_t>(rows));
        for (std::int64_t i = 0; i <= rows; ++i)
            offs[static_cast<std::size_t>(i)] =
                static_cast<std::int32_t>(i * len);
        for (std::size_t i = 0; i < items.size(); ++i)
            items[i] = static_cast<std::int64_t>(i);
        for (std::int64_t i = 0; i < rows; ++i)
            id[static_cast<std::size_t>(i)] = i;
        DataFrame df;
        df.names = {"id", "xs"};
        df.columns.push_back(Series::flat_i64(id.data(), rows));
        df.columns.push_back(Series::list(
            offs, Series::flat_i64(items.data(),
                                   static_cast<std::int64_t>(items.size()))));
        const auto whole = morsels(df.lazy().explode("xs"));
        const auto cut =
            morsels(df.lazy().memory_budget(256 << 10).explode("xs"));
        REQUIRE(most_rows(whole) == rows * len);
        CHECK(most_rows(cut) < 2000);
        CHECK(cut.size() > 40);
        expect_same_rows(joined(cut), joined(whole));
    }

    TEST_CASE("unpivot of many value columns leaves in morsels of the share") {
        std::vector<std::string> names = {"id"};
        for (int c = 0; c < 10; ++c) names.push_back("v" + std::to_string(c));
        const std::vector<std::string> vals(names.begin() + 1, names.end());
        const DataFrame df = ints(names, 4000, 97);
        const auto whole = morsels(df.lazy().unpivot({"id"}, vals));
        const auto cut =
            morsels(df.lazy().memory_budget(256 << 10).unpivot({"id"}, vals));
        REQUIRE(most_rows(whole) == 40000);
        CHECK(most_rows(cut) < 1500);
        // An unpivot lists a morsel by value column, so the order between
        // morsels follows their size; within a value column the rows keep the
        // order of the input.
        expect_same_rows(joined(cut).sort_by("variable", false),
                         joined(whole).sort_by("variable", false));
    }

    TEST_CASE("a join with many matches leaves in morsels of the share") {
        // 300 right rows match each left row: 180000 output rows.
        DataFrame left = ints({"k", "a"}, 600, 6);
        DataFrame right = ints({"k", "b"}, 600, 6);
        const auto whole = morsels(left.lazy().join(right.lazy(), {"k"}));
        const auto cut = morsels(
            left.lazy().memory_budget(128 << 10).join(right.lazy(), {"k"}));
        REQUIRE(row_set(whole).size() == 180000);
        CHECK(most_rows(cut) < 6000);
        CHECK(row_set(cut) == row_set(whole));
    }

    TEST_CASE("an outer join flushes its unmatched right rows in morsels") {
        DataFrame left = ints({"k", "a"}, 10, 1000);
        DataFrame right = ints({"k", "b"}, 5000, 100000);
        const auto cut = morsels(left.lazy().memory_budget(64 << 10).join(
            right.lazy(), {"k"}, JoinHow::Right));
        const auto whole =
            morsels(left.lazy().join(right.lazy(), {"k"}, JoinHow::Right));
        CHECK(most_rows(whole) >= 4000);
        CHECK(most_rows(cut) < 1500);
        CHECK(row_set(cut) == row_set(whole));
    }

    TEST_CASE("a cross join whose right side is over the budget spools it") {
        const DataFrame left = ints({"a", "b"}, 100, 50);
        const DataFrame right = ints({"c", "d", "e"}, 1500, 70);
        // The right side is 36000 bytes; a 64 KiB budget keeps a quarter of
        // that in memory.
        const std::uint64_t before = cross_join_spooled_rows();
        const auto cut = morsels(left.lazy().memory_budget(64 << 10).join(
            right.lazy(), {}, JoinHow::Cross));
        const DataFrame want = left.join(right, {}, JoinHow::Cross);
        REQUIRE(want.num_rows() == 150000);
        CHECK(cross_join_spooled_rows() - before == 1500);
        CHECK(most_rows(cut) < 3000);
        CHECK(row_set(cut) == row_set(want));
        // A right side that fits is not spooled.
        const std::uint64_t mid = cross_join_spooled_rows();
        const DataFrame small = ints({"c"}, 50, 7);
        const auto kept = morsels(left.lazy().memory_budget(64 << 10).join(
            small.lazy(), {}, JoinHow::Cross));
        CHECK(cross_join_spooled_rows() == mid);
        CHECK(row_set(kept) == row_set(left.join(small, {}, JoinHow::Cross)));
    }

    TEST_CASE(
        "take and group_by_dynamic leave in morsels of at most max rows") {
        const DataFrame df = ints({"t", "v"}, 6000, 5000);
        std::vector<std::int64_t> idx;
        for (std::int64_t i = 0; i < 5000; ++i) idx.push_back(5999 - i);
        const auto taken = morsels(df.lazy().take(idx), 1000);
        CHECK(most_rows(taken) <= 1000);
        CHECK(taken.size() >= 5);
        expect_same_rows(joined(taken), df.take(idx));
        // group_by_dynamic needs an ascending time: t = 3 * row.
        const DataFrame tf = ints({"t", "v"}, 6000, std::int64_t{1} << 40);
        const std::vector<GroupAgg> aggs = {GroupAgg{Agg::Sum, "v", "s"}};
        const auto windows =
            morsels(tf.lazy().group_by_dynamic("t", 7, 7, aggs), 500);
        CHECK(most_rows(windows) <= 500);
        CHECK(windows.size() >= 2);
        expect_same_rows(joined(windows), tf.group_by_dynamic("t", 7, 7, aggs));
    }

    TEST_CASE("group_by_dynamic refuses a time that goes back") {
        std::vector<std::int64_t> t{1, 5, 9, 4, 12};
        std::vector<std::int64_t> v{1, 1, 1, 1, 1};
        DataFrame df;
        df.names = {"t", "v"};
        df.columns.push_back(Series::flat_i64(t.data(), 5));
        df.columns.push_back(Series::flat_i64(v.data(), 5));
        const std::vector<GroupAgg> aggs = {GroupAgg{Agg::Count, "", "n"}};
        CHECK_THROWS_WITH_AS(df.group_by_dynamic("t", 2, 4, aggs),
                             "group_by_dynamic: t must ascend, 4 follows 9",
                             std::invalid_argument);
        CHECK_THROWS_WITH_AS(
            morsels(df.lazy().memory_budget(1 << 20).group_by_dynamic("t", 2, 4,
                                                                      aggs),
                    2),
            "group_by_dynamic: t must ascend, 4 follows 9",
            std::invalid_argument);
    }

    TEST_CASE("an elementwise column op runs one morsel at a time") {
        using dftracer::utils::dataframe::OpArgs;
        const DataFrame df = ints({"t", "v"}, 6000, 5000);
        dftu_scalar five{};
        five.kind = DFTU_SCALAR_TAG_I64;
        five.value.i = 5;
        dftu_scalar zero = five;
        zero.value.i = 0;
        auto column_op = [&](const char* op, dftu_scalar a) {
            OpArgs args;
            args.str(1, "v")
                .str(2, op)
                .str(3, "")
                .scalar(4, a)
                .scalar(5, zero)
                .str(6, "");
            return df.lazy().frame_op("dftu.frame.column_op", args, {},
                                      df.names);
        };
        const auto add =
            morsels(column_op("dftu.series.add_scalar", five), 1000);
        CHECK(add.size() >= 6);
        CHECK(most_rows(add) <= 1000);
        const DataFrame whole_add =
            joined(morsels(column_op("dftu.series.add_scalar", five)));
        expect_same_rows(joined(add), whole_add);
        // A running sum needs the whole column: one result, whole.
        const auto sum = morsels(column_op("dftu.series.cumsum", zero), 1000);
        const DataFrame whole_sum =
            joined(morsels(column_op("dftu.series.cumsum", zero)));
        expect_same_rows(joined(sum), whole_sum);
        CHECK(whole_sum.num_rows() == 6000);
    }

    TEST_CASE("every elementwise column op is a registered column op") {
        for (const std::string_view name :
             lazy_internal::ELEMENTWISE_COLUMN_OPS) {
            const dftu_op_desc* op = dftu_op_find(std::string(name).c_str());
            REQUIRE_MESSAGE(op, name);
            CHECK_MESSAGE(dftu_op_kind_of(op->sig) == DFTU_OP_KIND_SERIES,
                          name);
        }
    }

    TEST_CASE("a frame op over an input with no rows gets typed columns") {
        using dftracer::utils::dataframe::OpArgs;
        const DataFrame none = ints({"t", "v"}, 0, 7);
        dftu_scalar zero{};
        zero.kind = DFTU_SCALAR_TAG_I64;
        OpArgs args;
        args.str(1, "v")
            .str(2, "dftu.series.cumsum")
            .str(3, "")
            .scalar(4, zero)
            .scalar(5, zero)
            .str(6, "");
        // A running sum needs the whole column, so the input is drained first.
        const auto parts = morsels(
            none.lazy().frame_op("dftu.frame.column_op", args, {}, none.names),
            64);
        REQUIRE(parts.size() == 1);
        CHECK(parts[0].num_rows() == 0);
        CHECK(parts[0].names == none.names);
        REQUIRE(parts[0].columns.size() == 2);
        CHECK(parts[0].columns[1].type() == TypeId::Int64);
    }

    TEST_CASE("a grouped result with no rows still leaves with its columns") {
        const DataFrame none = ints({"t", "v"}, 0, 7);
        const std::vector<GroupAgg> aggs = {GroupAgg{Agg::Sum, "v", "s"}};
        const std::vector<std::string> want = {"t", "s"};
        for (const std::uint64_t budget :
             {std::uint64_t{0}, std::uint64_t{1} << 20}) {
            INFO("budget=" << budget);
            const LazyFrame in =
                budget ? none.lazy().memory_budget(budget) : none.lazy();
            for (const LazyFrame& lf :
                 {in.group_by(std::vector<std::string>{"t"}, aggs),
                  in.group_by_dynamic("t", 7, 7, aggs)}) {
                const auto parts = morsels(lf, 64);
                REQUIRE(parts.size() == 1);
                CHECK(parts[0].num_rows() == 0);
                CHECK(parts[0].names == want);
                CHECK(parts[0].columns.size() == want.size());
            }
        }
    }

    TEST_CASE("group_by_dynamic with more windows than the budget is eager") {
        // t = 3 * row ascends; v differs per row, so first and last show the
        // row order inside each window.
        const DataFrame df = ints({"t", "v"}, 20000, std::int64_t{1} << 40);
        const std::vector<GroupAgg> aggs = {
            GroupAgg{Agg::Sum, "v", "s"}, GroupAgg{Agg::Count, "v", "n"},
            GroupAgg{Agg::Min, "t", "lo"}, GroupAgg{Agg::First, "v", "f"},
            GroupAgg{Agg::Last, "v", "l"}};
        for (const auto& [every, period] :
             {std::pair<std::int64_t, std::int64_t>{1, 1}, {3, 3}, {2, 7}}) {
            INFO("every=" << every << " period=" << period);
            const auto got =
                morsels(df.lazy().memory_budget(64 << 10).group_by_dynamic(
                            "t", every, period, aggs),
                        500);
            expect_same_rows(joined(got),
                             df.group_by_dynamic("t", every, period, aggs));
        }
    }

    TEST_CASE("a topk whose rows outgrow the budget is cut from a sort") {
        const DataFrame df = ints({"a", "b"}, 4000, 100);
        for (const bool largest : {true, false}) {
            const auto cut = morsels(
                df.lazy().memory_budget(64 << 10).topk("a", 3000, largest),
                500);
            // The running best leaves as one morsel; the sort in several.
            CHECK(cut.size() > 1);
            expect_same_rows(joined(cut), df.topk("a", 3000, largest));
            const auto kept = morsels(
                df.lazy().memory_budget(64 << 20).topk("a", 3000, largest),
                500);
            CHECK(kept.size() == 1);
            expect_same_rows(joined(kept), df.topk("a", 3000, largest));
        }
    }

    TEST_CASE("a whole-column op under a budget sees the whole input") {
        using dftracer::utils::dataframe::OpArgs;
        DataFrame df = ints({"t"}, 6000, 5000);
        std::vector<double> f(6000);
        for (std::size_t i = 0; i < f.size(); ++i)
            f[i] = static_cast<double>(i % 97) * 0.1;
        df.names.push_back("v");
        df.columns.push_back(Series::flat_f64(f.data(), 6000));
        dftu_scalar zero{};
        zero.kind = DFTU_SCALAR_TAG_I64;
        OpArgs args;
        args.str(1, "v")
            .str(2, "dftu.series.cumsum")
            .str(3, "")
            .scalar(4, zero)
            .scalar(5, zero)
            .str(6, "");
        const std::uint64_t before = column_scan_morsels();
        const auto cut = morsels(df.lazy().memory_budget(64 << 10).frame_op(
            "dftu.frame.column_op", args, {}, df.names));
        CHECK(column_scan_morsels() == before);
        const auto whole = morsels(
            df.lazy().frame_op("dftu.frame.column_op", args, {}, df.names));
        expect_same_rows(joined(cut), joined(whole));
    }

    TEST_CASE("a running or positional column op runs one morsel at a time") {
        using dftracer::utils::dataframe::OpArgs;
        constexpr std::int64_t ROWS = 6000;
        // Int64 with nulls in runs (a leading run, a run across a morsel
        // edge), Int32 and Float64.
        std::vector<std::int64_t> a(ROWS);
        std::vector<std::int32_t> b(ROWS);
        std::vector<double> c(ROWS);
        std::vector<std::uint8_t> valid((ROWS + 7) / 8, 0xFF);
        for (std::int64_t i = 0; i < ROWS; ++i) {
            const auto k = static_cast<std::size_t>(i);
            a[k] = (i * 7919) % 1000 - 500;
            b[k] = 300'000 - static_cast<std::int32_t>(i % 13) * 1000;
            c[k] = static_cast<double>(i % 41) * 0.5;
            if (i < 3 || (i >= 990 && i < 1010) || i % 250 == 7)
                valid[k >> 3] &= static_cast<std::uint8_t>(~(1u << (k & 7)));
        }
        DataFrame df;
        df.names = {"t", "a", "b", "c"};
        df.columns.push_back(ints({"t"}, ROWS, 5000).columns[0].share());
        df.columns.push_back(Series::flat_i64(a.data(), ROWS, valid.data()));
        df.columns.push_back(
            Series::flat(TypeId::Int32, b.data(), ROWS, valid.data()));
        df.columns.push_back(Series::flat_f64(c.data(), ROWS, valid.data()));
        dftu_scalar zero{};
        zero.kind = DFTU_SCALAR_TAG_I64;
        auto plan = [&](const char* col, const char* op, std::int64_t n,
                        bool budget) {
            dftu_scalar arg = zero;
            arg.value.i = n;
            OpArgs args;
            args.str(1, col)
                .str(2, op)
                .str(3, "")
                .scalar(4, arg)
                .scalar(5, zero)
                .str(6, "");
            LazyFrame lf = df.lazy();
            if (budget) lf = lf.memory_budget(64 << 10);
            return lf.frame_op("dftu.frame.column_op", args, {}, df.names);
        };
        struct Case {
            const char* column;
            const char* op;
            std::int64_t n;
            bool streams;
        };
        const Case cases[] = {
            {"a", "dftu.series.cumsum", 0, true},
            {"b", "dftu.series.cumsum", 0, true},
            {"a", "dftu.series.cum_prod", 0, true},
            {"a", "dftu.series.cummax", 0, true},
            {"b", "dftu.series.cummin", 0, true},
            {"a", "dftu.series.ffill", 0, true},
            {"c", "dftu.series.ffill", 0, true},
            {"a", "dftu.series.diff", 0, true},
            {"b", "dftu.series.diff", 0, true},
            {"c", "dftu.series.diff", 0, true},
            {"a", "dftu.series.shift", 0, true},
            {"a", "dftu.series.shift", 3, true},
            {"c", "dftu.series.shift", 1000, true},
            {"c", "dftu.series.shift", 1500, false},
            {"c", "dftu.series.cumsum", 0, false},
            {"c", "dftu.series.cummax", 0, false},
            {"a", "dftu.series.shift", -2, false},
        };
        for (const Case& k : cases) {
            CAPTURE(k.column);
            CAPTURE(k.op);
            CAPTURE(k.n);
            const std::uint64_t before = column_scan_morsels();
            const auto cut = morsels(plan(k.column, k.op, k.n, true), 1000);
            const std::uint64_t ran = column_scan_morsels() - before;
            if (k.streams) {
                CHECK(ran >= 6);
                CHECK(cut.size() >= 6);
                CHECK(most_rows(cut) <= 1000);
            } else {
                CHECK(ran == 0);
            }
            const DataFrame whole =
                joined(morsels(plan(k.column, k.op, k.n, false)));
            REQUIRE(whole.num_rows() == ROWS);
            expect_same_rows(joined(cut), whole);
        }
    }

    TEST_CASE("with_column of a column runs one morsel at a time") {
        using dftracer::utils::dataframe::OpArgs;
        const DataFrame df = ints({"t", "v"}, 6000, 5000);
        const DataFrame extra = ints({"w"}, 6000, 77);
        for (const char* name : {"w", "v"}) {
            OpArgs args;
            args.str(1, name).series(2, extra.columns[0].handle());
            auto plan = [&](bool budget) {
                LazyFrame lf = df.lazy();
                if (budget) lf = lf.memory_budget(64 << 10);
                return lf.frame_op("dftu.frame.with_column", args, {}, {});
            };
            const std::uint64_t before = column_scan_morsels();
            const auto cut = morsels(plan(true), 1000);
            CHECK(column_scan_morsels() - before >= 6);
            CHECK(cut.size() >= 6);
            CHECK(most_rows(cut) <= 1000);
            expect_same_rows(joined(cut),
                             df.with_column(name, extra.columns[0]));
        }
    }

    TEST_CASE("frame ops with a lazy twin or a lazy form do not collect") {
        using dftracer::utils::dataframe::OpArgs;
        const DataFrame df = ints({"t", "v", "w"}, 6000, 100);
        const auto streamed = [](const LazyFrame& lf) {
            return lf.explain().find("frame_op") == std::string::npos;
        };
        {
            std::vector<std::uint8_t> bits(6000 / 8 + 1, 0);
            for (std::int64_t i = 0; i < 6000; i += 3)
                bits[static_cast<std::size_t>(i >> 3)] |=
                    static_cast<std::uint8_t>(1u << (i & 7));
            const Series mask = Series::flat(TypeId::Bool, bits.data(), 6000);
            OpArgs args;
            args.series(1, mask.handle());
            const LazyFrame lf = df.lazy().frame_op("dftu.frame.filter", args);
            CHECK(streamed(lf));
            expect_same_rows(joined(morsels(lf, 1000)), df.filter(mask));
        }
        {
            const char* by[] = {"v", "w"};
            const std::int32_t flags[] = {1, 0};
            OpArgs args;
            args.strlist(1, by, 2).i32list(2, flags);
            const LazyFrame lf =
                df.lazy().frame_op("dftu.frame.sort_by_multi_per_col", args);
            CHECK(streamed(lf));
            expect_same_rows(
                joined(morsels(lf, 1000)),
                df.sort_by_multi({"v", "w"}, std::vector<bool>{true, false}));
        }
        {
            const std::vector<GroupAgg> aggs = {GroupAgg{Agg::Sum, "v", "s"}};
            const dftu_group_agg raw[] = {{"sum", "v", "s"}};
            OpArgs args;
            args.str(1, "t").i64(2, 7).i64(3, 7).agglist(4, raw);
            const DataFrame sorted = df.sort_by("t", false);
            const LazyFrame lf =
                sorted.lazy().frame_op("dftu.frame.group_by_dynamic", args);
            CHECK(streamed(lf));
            expect_same_rows(joined(morsels(lf, 500)),
                             sorted.group_by_dynamic("t", 7, 7, aggs));
        }
    }
}
