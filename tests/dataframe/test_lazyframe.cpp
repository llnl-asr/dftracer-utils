#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/agg_expr.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/frame_ops.h>
#include <dftracer/utils/dataframe/internal/cell_ops.h>
#include <dftracer/utils/dataframe/internal/native_transform.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/op.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using dftracer::utils::StringIntern;
using dftracer::utils::coro::CoroTask;
using dftracer::utils::dataframe::Agg;
using dftracer::utils::dataframe::agg_argmax;
using dftracer::utils::dataframe::agg_mean;
using dftracer::utils::dataframe::agg_sum;
using dftracer::utils::dataframe::AggExprSpec;
using dftracer::utils::dataframe::col;
using dftracer::utils::dataframe::Cursor;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::eval;
using dftracer::utils::dataframe::Expr;
using dftracer::utils::dataframe::GroupAgg;
using dftracer::utils::dataframe::GroupwiseOp;
using dftracer::utils::dataframe::LazyFrame;
using dftracer::utils::dataframe::Morsel;
using dftracer::utils::dataframe::OpArgs;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::Source;
using dftracer::utils::dataframe::TypeId;

namespace {

DataFrame run(CoroTask<DataFrame> t) {
    DataFrame out =
        dftracer::utils::default_runtime().submit(std::move(t)).get();
    for (Series& c : out.columns) c = c.materialize();
    return out;
}

// Same names, row count, column types, nulls and values. A NaN equals a NaN:
// its sign and payload depend on the arithmetic path.
void expect_frames_equal(const DataFrame& a, const DataFrame& b) {
    REQUIRE(a.names == b.names);
    REQUIRE(a.num_rows() == b.num_rows());
    for (std::size_t c = 0; c < a.columns.size(); ++c) {
        std::vector<Series> x, y;
        x.push_back(a.columns[c].materialize());
        y.push_back(b.columns[c].materialize());
        REQUIRE(x[0].type() == y[0].type());
        if (x[0].type() == TypeId::List) {
            const std::int32_t* xo = x[0].offsets();
            const std::int32_t* yo = y[0].offsets();
            std::vector<Series> xe, ye;
            xe.push_back(x[0].child(0).materialize());
            ye.push_back(y[0].child(0).materialize());
            for (std::int64_t r = 0; r < a.num_rows(); ++r) {
                REQUIRE(x[0].is_null(r) == y[0].is_null(r));
                REQUIRE(xo[r + 1] - xo[r] == yo[r + 1] - yo[r]);
                for (std::int32_t k = 0; k < xo[r + 1] - xo[r]; ++k)
                    REQUIRE(
                        dftracer::utils::dataframe::row_key(xe, xo[r] + k) ==
                        dftracer::utils::dataframe::row_key(ye, yo[r] + k));
            }
            continue;
        }
        for (std::int64_t r = 0; r < a.num_rows(); ++r) {
            REQUIRE(x[0].is_null(r) == y[0].is_null(r));
            if (x[0].is_null(r)) continue;
            if (x[0].type() == TypeId::Float64 &&
                std::isnan(x[0].data<double>()[r]) &&
                std::isnan(y[0].data<double>()[r]))
                continue;
            if (x[0].type() == TypeId::Float32 &&
                std::isnan(x[0].data<float>()[r]) &&
                std::isnan(y[0].data<float>()[r]))
                continue;
            REQUIRE(dftracer::utils::dataframe::row_key(x, r) ==
                    dftracer::utils::dataframe::row_key(y, r));
        }
    }
}

// Drains a Cursor to one positional DataFrame (columns keep no names). A large
// max_rows pulls the whole in-memory source as a single morsel.
CoroTask<DataFrame> drain_cursor(std::unique_ptr<Cursor> cur) {
    DataFrame out;
    while (auto m = co_await cur->next(1 << 20)) {
        out.columns = std::move(m->columns);
        out.names.assign(out.columns.size(), std::string());
    }
    co_return out;
}

CoroTask<std::pair<std::vector<std::int64_t>, DataFrame>> probe_stream(
    dftracer::utils::coro::AsyncGenerator<DataFrame> gen) {
    std::vector<std::int64_t> chunk_rows;
    std::vector<DataFrame> chunks;
    while (auto df = co_await gen.next()) {
        chunk_rows.push_back(df->num_rows());
        chunks.push_back(std::move(*df));
    }
    std::vector<const DataFrame*> ptrs;
    for (const DataFrame& c : chunks) ptrs.push_back(&c);
    DataFrame merged = dftracer::utils::dataframe::concat(
        ptrs, dftracer::utils::dataframe::ConcatHow::Vertical);
    co_return std::make_pair(std::move(chunk_rows), std::move(merged));
}

std::pair<std::vector<std::int64_t>, DataFrame> run_stream_probe(
    dftracer::utils::coro::AsyncGenerator<DataFrame> gen) {
    return dftracer::utils::default_runtime()
        .submit(probe_stream(std::move(gen)))
        .get();
}

CoroTask<std::vector<DataFrame>> stream_parts(
    dftracer::utils::coro::AsyncGenerator<DataFrame> gen) {
    std::vector<DataFrame> parts;
    while (auto df = co_await gen.next()) parts.push_back(std::move(*df));
    co_return parts;
}

std::vector<DataFrame> run_stream(const LazyFrame& lf, std::int64_t rows) {
    return dftracer::utils::default_runtime()
        .submit(stream_parts(lf.stream(rows)))
        .get();
}

DataFrame make_df() {
    std::vector<std::int64_t> a{1, 2, 3, 4, 5, 6};
    std::vector<std::int64_t> b{10, 20, 30, 40, 50, 60};
    DataFrame df;
    df.names = {"a", "b"};
    df.columns.push_back(Series::flat_i64(a.data(), 6));
    df.columns.push_back(Series::flat_i64(b.data(), 6));
    return df;
}

std::vector<const Series*> ptrs(const DataFrame& df) {
    std::vector<const Series*> in;
    for (const Series& c : df.columns) in.push_back(&c);
    return in;
}

// A Cursor whose two morsels carry differing schemas via name_ids, to drive
// the reconcile-by-name path of drain_to_frame.
class RaggedCursor : public Cursor {
   public:
    explicit RaggedCursor(std::shared_ptr<StringIntern> intern)
        : intern_(std::move(intern)) {}

    CoroTask<std::optional<Morsel>> next(std::int64_t) override {
        if (step_ == 0) {
            ++step_;
            std::vector<std::int64_t> a{1, 2};
            std::vector<std::int64_t> b{10, 20};
            Morsel m;
            m.columns.push_back(Series::flat_i64(a.data(), 2));
            m.columns.push_back(Series::flat_i64(b.data(), 2));
            m.rows = 2;
            m.dyn_state().name_ids = {intern_->get_or_insert("a"),
                                      intern_->get_or_insert("b")};
            m.dyn->intern = intern_;
            co_return m;
        }
        if (step_ == 1) {
            ++step_;
            std::vector<double> a{3.5};
            std::vector<std::string> c{"x"};
            Morsel m;
            m.columns.push_back(Series::flat_f64(a.data(), 1));
            m.columns.push_back(Series::strings(c));
            m.rows = 1;
            m.dyn_state().name_ids = {intern_->get_or_insert("a"),
                                      intern_->get_or_insert("c")};
            m.dyn->intern = intern_;
            co_return m;
        }
        co_return std::nullopt;
    }

   private:
    int step_ = 0;
    std::shared_ptr<StringIntern> intern_;
};

class RaggedSource : public Source {
   public:
    dftracer::utils::dataframe::Schema schema() const override {
        return {{dftracer::utils::dataframe::Field{
            "a",
            dftracer::utils::dataframe::scalar(
                dftracer::utils::dataframe::TypeId::Unknown),
            true}}};
    }
    dftracer::utils::dataframe::ScanResult scan(
        const dftracer::utils::dataframe::ScanRequest& req) const override {
        auto intern = std::make_shared<StringIntern>();
        dftracer::utils::dataframe::ScanResult r;
        r.cursor = std::make_unique<RaggedCursor>(std::move(intern));
        r.filters.assign(req.filters.size(),
                         dftracer::utils::dataframe::Pushed::No);
        return r;
    }
};

}  // namespace

TEST_SUITE("lazyframe") {
    TEST_CASE("lazy filter+with_column+select matches the eager path") {
        auto lf = make_df()
                      .lazy()
                      .filter(col(0) > std::int64_t{3})
                      .with_column("c", col(0) + col(1))
                      .select({"a", "c"});

        CHECK(lf.schema() == std::vector<std::string>{"a", "c"});

        // Small morsel size to exercise multi-morsel scan + concat.
        DataFrame lazy = run(lf.collect(2));

        DataFrame e = make_df();
        DataFrame ef = e.filter(eval(col(0) > std::int64_t{3}, ptrs(e)));
        DataFrame ew = ef.with_column("c", eval(col(0) + col(1), ptrs(ef)));
        DataFrame eager = ew.select({"a", "c"});

        REQUIRE(lazy.num_rows() == eager.num_rows());
        REQUIRE(lazy.num_columns() == eager.num_columns());
        CHECK(lazy.names == eager.names);
        CHECK(lazy.num_rows() == 3);  // a in {4,5,6}
        for (std::size_t c = 0; c < lazy.num_columns(); ++c) {
            const std::int64_t* lp = lazy.columns[c].data<std::int64_t>();
            const std::int64_t* ep = eager.columns[c].data<std::int64_t>();
            for (std::int64_t i = 0; i < lazy.num_rows(); ++i)
                CHECK(lp[i] == ep[i]);
        }
        const std::int64_t* cc = lazy.column("c").data<std::int64_t>();
        CHECK(cc[0] == 44);
        CHECK(cc[2] == 66);
    }

    TEST_CASE("fused map (in-memory engine) matches the eager path") {
        // collect() with no morsel size runs the whole-column in-memory engine,
        // which fuses filter+with_column+select into one pass. Result must
        // equal the eager chain.
        auto lf = make_df()
                      .lazy()
                      .filter(col(1) > std::int64_t{20})
                      .with_column("c", col(0) + col(1))
                      .select({"a", "c"});
        DataFrame r = run(lf.collect());  // in-memory, fused
        CHECK(r.names == std::vector<std::string>{"a", "c"});
        CHECK(r.num_rows() == 4);  // b in {30,40,50,60} -> a in {3,4,5,6}
        const std::int64_t* a = r.column("a").data<std::int64_t>();
        const std::int64_t* c = r.column("c").data<std::int64_t>();
        CHECK(a[0] == 3);
        CHECK(c[0] == 33);  // 3 + 30
        CHECK(a[3] == 6);
        CHECK(c[3] == 66);  // 6 + 60
    }

    TEST_CASE("lazy(df) free function and full-scan roundtrip") {
        DataFrame all =
            run(dftracer::utils::dataframe::lazy(make_df()).collect());
        CHECK(all.num_rows() == 6);
        CHECK(all.num_columns() == 2);
        CHECK(all.column("b").data<std::int64_t>()[5] == 60);
    }

    TEST_CASE(
        "predicate pushdown moves an independent filter before with_column") {
        // Filter on 'a' (col 0) is independent of the added 'c', so it moves
        // up.
        auto lf = make_df()
                      .lazy()
                      .with_column("c", col(0) + col(1))
                      .filter(col(0) > std::int64_t{3});
        const std::string plan = lf.explain();
        const auto fpos = plan.find("filter");
        const auto wpos = plan.find("with_column");
        CHECK(fpos != std::string::npos);
        CHECK(wpos != std::string::npos);
        CHECK(fpos < wpos);  // filter reordered before with_column

        DataFrame r = run(lf.collect());
        CHECK(r.num_rows() == 3);
        CHECK(r.column("c").data<std::int64_t>()[0] == 44);  // 4 + 40
    }

    TEST_CASE("predicate pushdown moves a filter before sort_by") {
        // sort_by only reorders rows, so a row-local filter hoists ahead of it
        // and the sort runs on the survivors only; the result is those rows in
        // sorted order.
        auto lf = make_df().lazy().sort_by("a", true).filter(col(0) >
                                                             std::int64_t{3});
        const std::string plan = lf.explain();
        const auto fpos = plan.find("filter");
        const auto spos = plan.find("sort");
        CHECK(fpos != std::string::npos);
        CHECK(spos != std::string::npos);
        CHECK(fpos < spos);  // filter reordered before sort_by

        DataFrame r = run(lf.collect());
        // survivors a>3 => {4,5,6}, sorted descending => {6,5,4}.
        CHECK(r.num_rows() == 3);
        const std::int64_t* a = r.column("a").data<std::int64_t>();
        CHECK(a[0] == 6);
        CHECK(a[1] == 5);
        CHECK(a[2] == 4);
    }

    TEST_CASE("projection pushdown drops unread source columns") {
        // 'b' is neither read by the filter nor in the output, so a projection
        // to [a] is inserted right after the source, ahead of the filter.
        auto lf =
            make_df().lazy().filter(col(0) > std::int64_t{2}).select({"a"});
        const std::string plan = lf.explain();
        const auto proj = plan.find("select [a]");
        const auto filt = plan.find("filter");
        CHECK(proj != std::string::npos);
        CHECK(filt != std::string::npos);
        CHECK(proj < filt);  // projection pushed ahead of the filter

        DataFrame r = run(lf.collect());
        CHECK(r.names == std::vector<std::string>{"a"});
        CHECK(r.num_rows() == 4);  // a in {3,4,5,6}
        const std::int64_t* a = r.column("a").data<std::int64_t>();
        CHECK(a[0] == 3);
        CHECK(a[3] == 6);
    }

    TEST_CASE("projection pushdown remaps a filter on a surviving column") {
        // Filter reads 'b' (col 1) but output is 'a'; both are live, and the
        // remapped predicate must still select the right rows after the source
        // projection renumbers columns.
        auto lf =
            make_df().lazy().filter(col(1) > std::int64_t{30}).select({"a"});
        DataFrame r = run(lf.collect());
        CHECK(r.names == std::vector<std::string>{"a"});
        CHECK(r.num_rows() == 3);  // b in {40,50,60} -> a in {4,5,6}
        const std::int64_t* a = r.column("a").data<std::int64_t>();
        CHECK(a[0] == 4);
        CHECK(a[2] == 6);
    }

    TEST_CASE("streaming row ops: head / slice / tail / rename") {
        LazyFrame base = make_df().lazy();           // a=1..6, b=10..60

        DataFrame h = run(base.head(3).collect(2));  // morsel 2 -> multi-morsel
        CHECK(h.num_rows() == 3);
        CHECK(h.column("a").data<std::int64_t>()[0] == 1);
        CHECK(h.column("a").data<std::int64_t>()[2] == 3);

        DataFrame s = run(base.slice(2, 3).collect(2));  // rows a=3,4,5
        CHECK(s.num_rows() == 3);
        CHECK(s.column("a").data<std::int64_t>()[0] == 3);
        CHECK(s.column("a").data<std::int64_t>()[2] == 5);

        DataFrame open = run(
            base.slice(4, std::numeric_limits<std::int64_t>::max()).collect(2));
        CHECK(open.num_rows() == 2);
        CHECK(open.column("a").data<std::int64_t>()[0] == 5);

        DataFrame t = run(base.tail(2).collect(2));  // a=5,6
        CHECK(t.num_rows() == 2);
        CHECK(t.column("a").data<std::int64_t>()[0] == 5);
        CHECK(t.column("a").data<std::int64_t>()[1] == 6);

        DataFrame r = run(base.rename({"x", "y"}).collect());
        CHECK(r.names == std::vector<std::string>{"x", "y"});
        CHECK(r.column("x").data<std::int64_t>()[5] == 6);
    }

    TEST_CASE("streaming fill_null / drop_nulls / with_row_index") {
        std::vector<std::int64_t> a{1, 2, 3, 4};
        std::uint8_t bm = 0b1011;  // rows 0,1,3 valid; row 2 null
        DataFrame df;
        df.names = {"a"};
        df.columns.push_back(Series::flat_i64(a.data(), 4, &bm));

        DataFrame f = run(df.lazy().fill_null(std::int64_t{-1}).collect());
        CHECK(f.num_rows() == 4);
        CHECK(f.column("a").data<std::int64_t>()[2] == -1);

        DataFrame d = run(df.lazy().drop_nulls().collect());
        CHECK(d.num_rows() == 3);

        DataFrame w = run(make_df().lazy().with_row_index("idx").collect(2));
        CHECK(w.names[0] == "idx");
        CHECK(w.column("idx").data<std::int64_t>()[0] == 0);
        CHECK(w.column("idx").data<std::int64_t>()[5] == 5);

        // null_count over the nullable column (1 null), streamed at morsel 2.
        DataFrame nc = run(df.lazy().null_count().collect(2));
        CHECK(nc.num_rows() == 1);
        CHECK(nc.column("a").data<std::int64_t>()[0] == 1);
    }

    TEST_CASE("lazy topk and unpivot") {
        DataFrame tk =
            run(make_df().lazy().topk("a", 2).collect(2));  // largest 2 a
        CHECK(tk.num_rows() == 2);
        const std::int64_t* a = tk.column("a").data<std::int64_t>();
        CHECK(((a[0] == 6 && a[1] == 5) || (a[0] == 5 && a[1] == 6)));

        DataFrame up = run(make_df().lazy().unpivot({"a"}, {"b"}).collect(2));
        CHECK(up.names == std::vector<std::string>{"a", "variable", "value"});
        CHECK(up.num_rows() == 6);
    }

    TEST_CASE("streaming group_by matches eager (mergeable across morsels)") {
        std::vector<std::int64_t> g{0, 1, 0, 1, 0, 1};
        std::vector<std::int64_t> v{1, 2, 3, 4, 5, 6};
        DataFrame df;
        df.names = {"g", "v"};
        df.columns.push_back(Series::flat_i64(g.data(), 6));
        df.columns.push_back(Series::flat_i64(v.data(), 6));
        std::vector<GroupAgg> aggs{{Agg::Sum, "v", "sum", 0.0},
                                   {Agg::Mean, "v", "mean", 0.0},
                                   {Agg::Count, "", "count", 0.0}};

        // Small morsel size: mean must NOT be a mean-of-means.
        DataFrame r = run(df.lazy().group_by("g", aggs).collect(2));
        REQUIRE(r.num_rows() == 2);
        const std::int64_t* gk = r.column("g").data<std::int64_t>();
        const std::int64_t* sum = r.column("sum").data<std::int64_t>();
        const double* mean = r.column("mean").data<double>();
        const std::int64_t* cnt = r.column("count").data<std::int64_t>();
        for (std::int64_t i = 0; i < 2; ++i) {
            if (gk[i] == 0) {
                CHECK(sum[i] == 9);  // 1+3+5
                CHECK(mean[i] == doctest::Approx(3.0));
                CHECK(cnt[i] == 3);
            } else {
                CHECK(sum[i] == 12);  // 2+4+6
                CHECK(mean[i] == doctest::Approx(4.0));
                CHECK(cnt[i] == 3);
            }
        }
    }

    TEST_CASE(
        "streaming group_by(vector<string>) composite key across morsels") {
        std::vector<std::int64_t> g{0, 1, 0, 1, 0, 1};
        std::vector<std::int64_t> p{1, 1, 1, 2, 1, 2};
        std::vector<std::int64_t> v{1, 2, 3, 4, 5, 6};
        DataFrame df;
        df.names = {"g", "p", "v"};
        df.columns.push_back(Series::flat_i64(g.data(), 6));
        df.columns.push_back(Series::flat_i64(p.data(), 6));
        df.columns.push_back(Series::flat_i64(v.data(), 6));
        std::vector<GroupAgg> aggs{{Agg::Sum, "v", "sum", 0.0},
                                   {Agg::Count, "", "count", 0.0}};

        // Small morsel size forces multiple partial states to merge.
        DataFrame r =
            run(df.lazy()
                    .group_by(std::vector<std::string>{"g", "p"}, aggs)
                    .collect(2));
        DataFrame expected =
            df.group_by(std::vector<std::string>{"g", "p"}, aggs);
        REQUIRE(r.names == std::vector<std::string>{"g", "p", "sum", "count"});
        REQUIRE(r.num_rows() == expected.num_rows());
        for (std::int64_t i = 0; i < r.num_rows(); ++i) {
            const std::int64_t gk = r.column("g").data<std::int64_t>()[i];
            const std::int64_t pk = r.column("p").data<std::int64_t>()[i];
            std::int64_t j = -1;
            for (std::int64_t k = 0; k < expected.num_rows(); ++k)
                if (expected.column("g").data<std::int64_t>()[k] == gk &&
                    expected.column("p").data<std::int64_t>()[k] == pk)
                    j = k;
            REQUIRE(j >= 0);
            CHECK(r.column("sum").data<std::int64_t>()[i] ==
                  expected.column("sum").data<std::int64_t>()[j]);
            CHECK(r.column("count").data<std::int64_t>()[i] ==
                  expected.column("count").data<std::int64_t>()[j]);
        }
    }

    TEST_CASE(
        "group_by external spill (tiny budget) matches in-memory, "
        "multi-key, several agg types") {
        constexpr std::int64_t N = 600;
        std::vector<std::int64_t> g(static_cast<std::size_t>(N)),
            p(static_cast<std::size_t>(N)), v(static_cast<std::size_t>(N));
        for (std::int64_t i = 0; i < N; ++i) {
            g[static_cast<std::size_t>(i)] = i % 5;
            p[static_cast<std::size_t>(i)] = (i / 3) % 4;
            v[static_cast<std::size_t>(i)] = i;
        }
        DataFrame df;
        df.names = {"g", "p", "v"};
        df.columns.push_back(Series::flat_i64(g.data(), N));
        df.columns.push_back(Series::flat_i64(p.data(), N));
        df.columns.push_back(Series::flat_i64(v.data(), N));

        std::vector<GroupAgg> aggs{
            {Agg::Sum, "v", "sum", 0.0},    {Agg::Mean, "v", "mean", 0.0},
            {Agg::Count, "", "count", 0.0}, {Agg::Min, "v", "min", 0.0},
            {Agg::Max, "v", "max", 0.0},    {Agg::Var, "v", "var", 0.0},
            {Agg::Pct, "v", "p50", 0.5}};
        const std::vector<std::string> keys{"g", "p"};

        DataFrame in_mem = run(df.lazy().group_by(keys, aggs).collect(64));
        // Tiny budget + small morsels: several AggState flushes, k-way merged
        // back on finalize.
        DataFrame spilled =
            run(df.lazy().memory_budget(256).group_by(keys, aggs).collect(8));

        DataFrame a = in_mem.sort_by_multi(keys);
        DataFrame b = spilled.sort_by_multi(keys);
        REQUIRE(a.names == b.names);
        REQUIRE(a.num_rows() == b.num_rows());
        for (std::int64_t i = 0; i < a.num_rows(); ++i) {
            CHECK(a.column("g").data<std::int64_t>()[i] ==
                  b.column("g").data<std::int64_t>()[i]);
            CHECK(a.column("p").data<std::int64_t>()[i] ==
                  b.column("p").data<std::int64_t>()[i]);
            CHECK(a.column("sum").data<std::int64_t>()[i] ==
                  b.column("sum").data<std::int64_t>()[i]);
            CHECK(a.column("count").data<std::int64_t>()[i] ==
                  b.column("count").data<std::int64_t>()[i]);
            CHECK(a.column("min").data<std::int64_t>()[i] ==
                  b.column("min").data<std::int64_t>()[i]);
            CHECK(a.column("max").data<std::int64_t>()[i] ==
                  b.column("max").data<std::int64_t>()[i]);
            CHECK(a.column("mean").data<double>()[i] ==
                  doctest::Approx(b.column("mean").data<double>()[i]));
            CHECK(a.column("var").data<double>()[i] ==
                  doctest::Approx(b.column("var").data<double>()[i]));
            CHECK(a.column("p50").data<double>()[i] ==
                  doctest::Approx(b.column("p50").data<double>()[i])
                      .epsilon(0.05));
        }
    }

    TEST_CASE("group_by dyn resident collect() matches the streaming path") {
        using dftracer::utils::dataframe::AggDynSpec;
        using dftracer::utils::dataframe::AggOp;
        std::vector<std::int64_t> g{0, 0, 1, 1};
        std::vector<double> x{1, 2, 3, 4};
        std::vector<double> y{10, 20, 30, 40};
        DataFrame df;
        df.names = {"g", "arg.x", "arg.y"};
        df.columns.push_back(Series::flat_i64(g.data(), 4));
        df.columns.push_back(Series::flat_f64(x.data(), 4));
        df.columns.push_back(Series::flat_f64(y.data(), 4));

        std::vector<GroupAgg> aggs{{Agg::Count, "", "n", 0.0}};
        std::vector<AggDynSpec> dyn{{AggOp::Sum, 0.0, "sum_"}};
        const std::vector<std::string> keys{"g"};

        // collect() (no morsel size, no budget) takes the resident whole-column
        // path; collect(2) forces the streaming GroupByCursor, which honors
        // dyn.
        DataFrame resident =
            run(df.lazy().group_by(keys, aggs, dyn, "arg.").collect());
        DataFrame streamed =
            run(df.lazy().group_by(keys, aggs, dyn, "arg.").collect(2));

        DataFrame a = resident.sort_by_multi(keys);
        DataFrame b = streamed.sort_by_multi(keys);
        REQUIRE(a.names ==
                b.names);  // dyn columns must survive the resident path
        REQUIRE(a.column_index("sum_x") >= 0);
        REQUIRE(a.num_rows() == b.num_rows());
        for (std::int64_t i = 0; i < a.num_rows(); ++i) {
            CHECK(a.column("n").data<std::int64_t>()[i] ==
                  b.column("n").data<std::int64_t>()[i]);
            CHECK(a.column("sum_x").data<double>()[i] ==
                  doctest::Approx(b.column("sum_x").data<double>()[i]));
            CHECK(a.column("sum_y").data<double>()[i] ==
                  doctest::Approx(b.column("sum_y").data<double>()[i]));
        }
    }

    TEST_CASE("group_by dyn that spills gives every part all dyn columns") {
        using dftracer::utils::dataframe::AggDynSpec;
        using dftracer::utils::dataframe::AggOp;
        constexpr std::int64_t N = 4000;
        // arg.z has values only in the last rows, so only the last flushes of
        // the spill see the name; every part must still carry sum_z.
        constexpr std::int64_t Z_FROM = N - 100;
        std::vector<std::int64_t> k(static_cast<std::size_t>(N));
        std::vector<double> x(static_cast<std::size_t>(N)),
            z(static_cast<std::size_t>(N), 0.0);
        std::vector<std::uint8_t> z_valid((N + 7) / 8, 0);
        for (std::int64_t i = 0; i < N; ++i) {
            k[static_cast<std::size_t>(i)] = i;
            x[static_cast<std::size_t>(i)] = static_cast<double>(i);
            if (i >= Z_FROM) {
                z[static_cast<std::size_t>(i)] = static_cast<double>(i * 3);
                z_valid[static_cast<std::size_t>(i >> 3)] |=
                    static_cast<std::uint8_t>(1u << (i & 7));
            }
        }
        DataFrame df;
        df.names = {"k", "arg.x", "arg.z"};
        df.columns.push_back(Series::flat_i64(k.data(), N));
        df.columns.push_back(Series::flat_f64(x.data(), N));
        df.columns.push_back(Series::flat_f64(z.data(), N, z_valid.data()));
        std::vector<GroupAgg> aggs{{Agg::Count, "", "n", 0.0}};
        std::vector<AggDynSpec> dyn{{AggOp::Sum, 0.0, "sum_"}};
        const std::vector<std::string> keys{"k"};
        const DataFrame in_mem =
            run(df.lazy().group_by(keys, aggs, dyn, "arg.").collect(256))
                .sort_by_multi(keys);
        REQUIRE(in_mem.column_index("sum_z") >= 0);

        const LazyFrame spill_plan =
            df.lazy().memory_budget(2000).group_by(keys, aggs, dyn, "arg.");
        std::vector<DataFrame> parts = run_stream(spill_plan, 16);
        REQUIRE(parts.size() > 1);
        for (const DataFrame& part : parts) REQUIRE(part.names == in_mem.names);
        std::vector<const DataFrame*> ptrs;
        for (const DataFrame& part : parts) ptrs.push_back(&part);
        const DataFrame spilled =
            dftracer::utils::dataframe::concat(
                ptrs, dftracer::utils::dataframe::ConcatHow::Vertical)
                .sort_by_multi(keys);
        REQUIRE(spilled.num_rows() == N);
        const Series& sz = spilled.column("sum_z");
        const Series& mz = in_mem.column("sum_z");
        for (std::int64_t i = 0; i < N; ++i) {
            CHECK(spilled.column("n").data<std::int64_t>()[i] ==
                  in_mem.column("n").data<std::int64_t>()[i]);
            CHECK(spilled.column("sum_x").data<double>()[i] ==
                  in_mem.column("sum_x").data<double>()[i]);
            REQUIRE(sz.is_null(i) == mz.is_null(i));
            if (!mz.is_null(i))
                CHECK(sz.data<double>()[i] == mz.data<double>()[i]);
        }
    }

    TEST_CASE(
        "group_by spill with high-cardinality keys triggers multiple "
        "flushes, matches eager") {
        constexpr std::int64_t N = 4000;
        std::vector<std::int64_t> k(static_cast<std::size_t>(N)),
            v(static_cast<std::size_t>(N));
        for (std::int64_t i = 0; i < N; ++i) {
            k[static_cast<std::size_t>(i)] = i;  // all-distinct: one row/group
            v[static_cast<std::size_t>(i)] = i * 2;
        }
        DataFrame df;
        df.names = {"k", "v"};
        df.columns.push_back(Series::flat_i64(k.data(), N));
        df.columns.push_back(Series::flat_i64(v.data(), N));

        std::vector<GroupAgg> aggs{{Agg::Sum, "v", "sum", 0.0},
                                   {Agg::Count, "", "count", 0.0}};

        DataFrame in_mem = run(df.lazy().group_by("k", aggs).collect(256));
        // Small budget relative to ~4000 groups forces many run flushes.
        DataFrame spilled =
            run(df.lazy().memory_budget(2000).group_by("k", aggs).collect(16));

        DataFrame a = in_mem.sort_by_multi({"k"});
        DataFrame b = spilled.sort_by_multi({"k"});
        REQUIRE(a.num_rows() == N);
        REQUIRE(b.num_rows() == N);
        for (std::int64_t i = 0; i < N; ++i) {
            CHECK(a.column("k").data<std::int64_t>()[i] ==
                  b.column("k").data<std::int64_t>()[i]);
            CHECK(a.column("sum").data<std::int64_t>()[i] ==
                  b.column("sum").data<std::int64_t>()[i]);
            CHECK(a.column("count").data<std::int64_t>()[i] ==
                  b.column("count").data<std::int64_t>()[i]);
        }
    }

    TEST_CASE(
        "group_by(vector<Expr>, AggExprSpec) matches the string overload") {
        std::vector<std::int64_t> g{0, 1, 0, 1, 0, 1};
        std::vector<std::int64_t> p{1, 1, 1, 2, 1, 2};
        std::vector<std::int64_t> v{1, 2, 3, 4, 5, 6};
        DataFrame df;
        df.names = {"g", "p", "v"};
        df.columns.push_back(Series::flat_i64(g.data(), 6));
        df.columns.push_back(Series::flat_i64(p.data(), 6));
        df.columns.push_back(Series::flat_i64(v.data(), 6));

        std::vector<GroupAgg> str_aggs{{Agg::Sum, "v", "s", 0.0}};
        DataFrame expected =
            run(df.lazy()
                    .group_by(std::vector<std::string>{"g", "p"}, str_aggs)
                    .collect(2));

        std::vector<AggExprSpec> expr_aggs{agg_sum(col(2), "s")};
        DataFrame got =
            run(df.lazy()
                    .group_by(std::vector<Expr>{col(0), col(1)}, expr_aggs)
                    .collect(2));

        REQUIRE(got.names == expected.names);
        REQUIRE(got.num_rows() == expected.num_rows());
        for (std::int64_t i = 0; i < got.num_rows(); ++i) {
            const std::int64_t gk = got.column("g").data<std::int64_t>()[i];
            const std::int64_t pk = got.column("p").data<std::int64_t>()[i];
            std::int64_t j = -1;
            for (std::int64_t k = 0; k < expected.num_rows(); ++k)
                if (expected.column("g").data<std::int64_t>()[k] == gk &&
                    expected.column("p").data<std::int64_t>()[k] == pk)
                    j = k;
            REQUIRE(j >= 0);
            CHECK(got.column("s").data<std::int64_t>()[i] ==
                  expected.column("s").data<std::int64_t>()[j]);
        }
    }

    TEST_CASE("group_by(Expr, AggExprSpec) matches the string overload") {
        std::vector<std::int64_t> g{0, 1, 0, 1, 0, 1};
        std::vector<std::int64_t> v{1, 2, 3, 4, 5, 6};
        DataFrame df;
        df.names = {"g", "v"};
        df.columns.push_back(Series::flat_i64(g.data(), 6));
        df.columns.push_back(Series::flat_i64(v.data(), 6));

        // A string-literal key must still resolve to the string overload, not
        // the Expr one (Expr has no implicit ctor from a string/char*).
        std::vector<GroupAgg> str_aggs{{Agg::Sum, "v", "s", 0.0},
                                       {Agg::Mean, "v", "m", 0.0}};
        DataFrame expected = run(df.lazy().group_by("g", str_aggs).collect(2));

        std::vector<AggExprSpec> expr_aggs{agg_sum(col(1), "s"),
                                           agg_mean(col(1), "m")};
        DataFrame got = run(df.lazy().group_by(col(0), expr_aggs).collect(2));

        REQUIRE(got.names == expected.names);
        REQUIRE(got.num_rows() == expected.num_rows());
        const std::int64_t* gk_e = expected.column("g").data<std::int64_t>();
        const std::int64_t* gk_g = got.column("g").data<std::int64_t>();
        const std::int64_t* s_e = expected.column("s").data<std::int64_t>();
        const std::int64_t* s_g = got.column("s").data<std::int64_t>();
        const double* m_e = expected.column("m").data<double>();
        const double* m_g = got.column("m").data<double>();
        for (std::int64_t i = 0; i < got.num_rows(); ++i) {
            CHECK(gk_g[i] == gk_e[i]);
            CHECK(s_g[i] == s_e[i]);
            CHECK(m_g[i] == doctest::Approx(m_e[i]));
        }
    }

    // Every agg_uses_by_col() op must get its `by` resolved by the Expr
    // desugar, not just ArgMax. ArgMin stands in for the whole set.
    TEST_CASE("group_by(Expr, ...) resolves `by` for a non-ArgMax by_col op") {
        std::vector<std::int64_t> g{0, 0, 1, 1, 0, 1};
        std::vector<std::int64_t> x{10, 20, 30, 40, 50, 60};
        std::vector<std::int64_t> by{5, 1, 9, 2, 3, 7};
        DataFrame df;
        df.names = {"g", "x", "by"};
        df.columns.push_back(Series::flat_i64(g.data(), 6));
        df.columns.push_back(Series::flat_i64(x.data(), 6));
        df.columns.push_back(Series::flat_i64(by.data(), 6));

        DataFrame ref =
            run(df.lazy()
                    .group_by("g", std::vector<GroupAgg>{{Agg::ArgMin, "x",
                                                          "am", 0.0, "by"}})
                    .collect(2));

        AggExprSpec spec;
        spec.op = dftracer::utils::dataframe::AggOp::ArgMin;
        spec.value = col(1);
        spec.by = col(2);
        spec.out = "am";
        DataFrame got =
            run(df.lazy()
                    .group_by(col(0), std::vector<AggExprSpec>{spec})
                    .collect(2));

        REQUIRE(got.num_rows() == ref.num_rows());
        const std::int64_t* rk = ref.column("g").data<std::int64_t>();
        const std::int64_t* gk = got.column(got.names[0]).data<std::int64_t>();
        for (std::int64_t i = 0; i < got.num_rows(); ++i) {
            bool matched = false;
            for (std::int64_t j = 0; j < ref.num_rows(); ++j) {
                if (rk[j] != gk[i]) continue;
                matched = true;
                CHECK(got.column("am").string_at(i) ==
                      ref.column("am").string_at(j));
            }
            CHECK(matched);
        }
    }

    TEST_CASE("group_by(Expr, ...) with a computed key and an ArgMax agg") {
        std::vector<std::int64_t> a{0, 0, 1, 1, 0, 1};
        std::vector<std::int64_t> b{0, 1, 0, 0, 1, 1};  // a+b: 0,1,1,1,1,2
        std::vector<std::int64_t> x{10, 20, 30, 40, 50, 60};
        std::vector<std::int64_t> by{5, 1, 9, 2, 3, 7};
        DataFrame df;
        df.names = {"a", "b", "x", "by"};
        df.columns.push_back(Series::flat_i64(a.data(), 6));
        df.columns.push_back(Series::flat_i64(b.data(), 6));
        df.columns.push_back(Series::flat_i64(x.data(), 6));
        df.columns.push_back(Series::flat_i64(by.data(), 6));

        // Hand-built with_column + string group_by: the reference desugar.
        DataFrame ref =
            run(df.lazy()
                    .with_column("__k", col(0) + col(1))
                    .group_by("__k",
                              std::vector<GroupAgg>{
                                  {Agg::Sum, "x", "s", 0.0},
                                  {Agg::ArgMax, "x", "am", 0.0, "by"}})
                    .collect(2));

        std::vector<AggExprSpec> specs{agg_sum(col(2), "s"),
                                       agg_argmax(col(2), col(3), "am")};
        DataFrame got =
            run(df.lazy().group_by(col(0) + col(1), specs).collect(2));

        REQUIRE(got.num_rows() == ref.num_rows());
        const std::int64_t* rk = ref.column("__k").data<std::int64_t>();
        const std::int64_t* rs = ref.column("s").data<std::int64_t>();
        const std::int64_t* gk = got.column(got.names[0]).data<std::int64_t>();
        const std::int64_t* gs = got.column("s").data<std::int64_t>();
        for (std::int64_t i = 0; i < got.num_rows(); ++i) {
            bool matched = false;
            for (std::int64_t j = 0; j < ref.num_rows(); ++j) {
                if (rk[j] != gk[i]) continue;
                matched = true;
                CHECK(gs[i] == rs[j]);
                CHECK(got.column("am").string_at(i) ==
                      ref.column("am").string_at(j));
            }
            CHECK(matched);
        }
    }

    TEST_CASE("lazy Expr-keyed group_by output schema matches eager") {
        std::vector<std::int64_t> a{0, 0, 1, 1, 0, 1};
        std::vector<std::int64_t> b{0, 1, 0, 0, 1, 1};
        std::vector<std::int64_t> x{10, 20, 30, 40, 50, 60};
        DataFrame df;
        df.names = {"a", "b", "x"};
        df.columns.push_back(Series::flat_i64(a.data(), 6));
        df.columns.push_back(Series::flat_i64(b.data(), 6));
        df.columns.push_back(Series::flat_i64(x.data(), 6));

        std::vector<AggExprSpec> specs{agg_sum(col(2), "s")};

        // Single computed key: named "key" (not the hidden __gb temp) both
        // ways.
        DataFrame eager1 = df.group_by(col(0) + col(1), specs);
        DataFrame lazy1 =
            run(df.lazy().group_by(col(0) + col(1), specs).collect(2));
        CHECK(lazy1.names == eager1.names);
        CHECK(lazy1.names[0] == "key");

        // N keys: one computed ("key0"), one bare column-ref ("a").
        DataFrame eager2 =
            df.group_by(std::vector<Expr>{col(0) + col(1), col(0)}, specs);
        DataFrame lazy2 =
            run(df.lazy()
                    .group_by(std::vector<Expr>{col(0) + col(1), col(0)}, specs)
                    .collect(2));
        CHECK(lazy2.names == eager2.names);
        CHECK(lazy2.names[0] == "key0");
        CHECK(lazy2.names[1] == "a");
    }

    TEST_CASE("sort_by (in-memory) and unique") {
        DataFrame s = run(make_df().lazy().sort_by("a", true).collect(2));
        CHECK(s.num_rows() == 6);
        CHECK(s.column("a").data<std::int64_t>()[0] == 6);
        CHECK(s.column("a").data<std::int64_t>()[5] == 1);

        std::vector<std::int64_t> d{1, 1, 2, 2, 3};
        DataFrame df;
        df.names = {"x"};
        df.columns.push_back(Series::flat_i64(d.data(), 5));
        DataFrame u = run(df.lazy().unique().collect(2));
        CHECK(u.num_rows() == 3);
    }

    TEST_CASE("streaming unique matches eager (first occurrence, order)") {
        // Duplicates spread across morsels; keep-first order must be preserved.
        std::vector<std::int64_t> x{5, 3, 5, 1, 3, 5, 2, 1};
        DataFrame df;
        df.names = {"x"};
        df.columns.push_back(Series::flat_i64(x.data(), 8));
        DataFrame lz = run(df.lazy().unique().collect(3));  // multi-morsel scan
        DataFrame eg = df.unique();
        REQUIRE(lz.num_rows() == eg.num_rows());
        const std::int64_t* lp = lz.column("x").data<std::int64_t>();
        const std::int64_t* ep = eg.column("x").data<std::int64_t>();
        for (std::int64_t i = 0; i < lz.num_rows(); ++i) CHECK(lp[i] == ep[i]);
        CHECK(lz.num_rows() == 4);  // {5,3,1,2}
    }

    TEST_CASE("unique external spill (tiny budget) matches in-memory") {
        std::vector<std::int64_t> x{5, 3, 5, 1, 3, 5, 2, 1, 7, 3, 9, 5};
        DataFrame df;
        df.names = {"x"};
        df.columns.push_back(
            Series::flat_i64(x.data(), static_cast<std::int64_t>(x.size())));

        DataFrame eg = df.unique();
        DataFrame lz =
            run(df.lazy().memory_budget(1).unique().collect(3));  // force spill

        REQUIRE(lz.num_rows() == eg.num_rows());
        const std::int64_t* lp = lz.column("x").data<std::int64_t>();
        const std::int64_t* ep = eg.column("x").data<std::int64_t>();
        // Identical order, not just an identical set: first occurrence in
        // original input order must survive the fast-path/spill boundary.
        for (std::int64_t i = 0; i < lz.num_rows(); ++i) CHECK(lp[i] == ep[i]);
        CHECK(lz.num_rows() == 6);  // {5,3,1,2,7,9}
    }

    TEST_CASE(
        "unique external spill with high-cardinality keys forces multiple "
        "partitions/recursion") {
        constexpr std::int64_t N = 5000;
        std::vector<std::int64_t> x(static_cast<std::size_t>(N));
        for (std::int64_t i = 0; i < N; ++i)
            x[static_cast<std::size_t>(i)] = i % 700;  // 700 distinct keys
        DataFrame df;
        df.names = {"x"};
        df.columns.push_back(Series::flat_i64(x.data(), N));

        DataFrame eg = df.unique();
        DataFrame lz = run(df.lazy().memory_budget(1).unique().collect(32));

        REQUIRE(lz.num_rows() == 700);
        REQUIRE(lz.num_rows() == eg.num_rows());
        const std::int64_t* lp = lz.column("x").data<std::int64_t>();
        const std::int64_t* ep = eg.column("x").data<std::int64_t>();
        for (std::int64_t i = 0; i < lz.num_rows(); ++i) CHECK(lp[i] == ep[i]);
        // No dupes: the 700 survivors are exactly 0..699 once each.
        std::vector<bool> found(700, false);
        for (std::int64_t i = 0; i < lz.num_rows(); ++i) {
            REQUIRE_FALSE(found[static_cast<std::size_t>(lp[i])]);
            found[static_cast<std::size_t>(lp[i])] = true;
        }
    }

    TEST_CASE(
        "unique().head(k) under a tiny budget returns first k distinct "
        "in order") {
        std::vector<std::int64_t> x{5, 3, 5, 1, 3, 5, 2, 1, 7, 9};
        DataFrame df;
        df.names = {"x"};
        df.columns.push_back(
            Series::flat_i64(x.data(), static_cast<std::int64_t>(x.size())));

        DataFrame lz =
            run(df.lazy().memory_budget(1).unique().head(3).collect(3));
        REQUIRE(lz.num_rows() == 3);
        const std::int64_t* lp = lz.column("x").data<std::int64_t>();
        CHECK(lp[0] == 5);
        CHECK(lp[1] == 3);
        CHECK(lp[2] == 1);
    }

    TEST_CASE("sort_by external merge (spilling) matches eager") {
        // Scrambled keys + a payload column, tiny budget + tiny morsels so the
        // sort spills several runs and k-way merges them back.
        std::vector<std::int64_t> k(200), v(200);
        for (std::int64_t i = 0; i < 200; ++i) {
            k[static_cast<std::size_t>(i)] =
                (i * 73 + 11) % 200;  // permutation
            v[static_cast<std::size_t>(i)] = i;
        }
        DataFrame df;
        df.names = {"k", "v"};
        df.columns.push_back(Series::flat_i64(k.data(), 200));
        df.columns.push_back(Series::flat_i64(v.data(), 200));

        for (bool desc : {false, true}) {
            DataFrame lz = run(df.lazy()
                                   .memory_budget(1024)  // force spilling
                                   .sort_by("k", desc)
                                   .collect(16));
            DataFrame eg = df.sort_by("k", desc);
            REQUIRE(lz.num_rows() == 200);
            const std::int64_t* lk = lz.column("k").data<std::int64_t>();
            const std::int64_t* ek = eg.column("k").data<std::int64_t>();
            for (std::int64_t i = 0; i < 200; ++i) CHECK(lk[i] == ek[i]);
            // Keys are a permutation (all distinct), so the payload aligns too.
            const std::int64_t* lv = lz.column("v").data<std::int64_t>();
            const std::int64_t* ev = eg.column("v").data<std::int64_t>();
            for (std::int64_t i = 0; i < 200; ++i) CHECK(lv[i] == ev[i]);
        }
    }

    TEST_CASE(
        "sort_by merges many spilled runs in passes and keeps ties stable") {
        // 30000 rows under a 4 KiB budget make far more runs than one merge
        // reads at once, so the runs are merged in groups first. Equal keys
        // must keep their input order.
        const std::int64_t n = 30000;
        std::vector<std::int64_t> k(n), v(n);
        for (std::int64_t i = 0; i < n; ++i) {
            k[static_cast<std::size_t>(i)] = (i * 7919) % 97;
            v[static_cast<std::size_t>(i)] = i;
        }
        DataFrame df;
        df.names = {"k", "v"};
        df.columns.push_back(Series::flat_i64(k.data(), n));
        df.columns.push_back(Series::flat_i64(v.data(), n));
        DataFrame lz =
            run(df.lazy().memory_budget(4096).sort_by("k").collect(512));
        REQUIRE(lz.num_rows() == n);
        const Series lk = lz.column("k").materialize();
        const Series lv = lz.column("v").materialize();
        for (std::int64_t i = 1; i < n; ++i) {
            const std::int64_t a = lk.data<std::int64_t>()[i - 1];
            const std::int64_t b = lk.data<std::int64_t>()[i];
            REQUIRE(a <= b);
            if (a == b)
                REQUIRE(lv.data<std::int64_t>()[i - 1] <
                        lv.data<std::int64_t>()[i]);
        }
    }

    TEST_CASE("a spilled sort orders every key type like the eager sort") {
        // Date, Timestamp and Float32 keys with nulls, NaN and signed zeros,
        // under a budget that makes several runs, in both directions, and
        // for empty and one-row inputs.
        const std::int64_t n = 600;
        std::vector<std::int32_t> date(n);
        std::vector<std::int64_t> ts(n), id(n);
        std::vector<float> fl(n);
        std::vector<std::uint8_t> valid((n + 7) / 8, 0);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float pool[6] = {-1.5f, -0.0f,
                               0.0f,  2.0f,
                               nan,   std::numeric_limits<float>::infinity()};
        for (std::int64_t i = 0; i < n; ++i) {
            const auto z = static_cast<std::size_t>(i);
            date[z] = static_cast<std::int32_t>((i * 389) % n) - 300;
            ts[z] = ((i * 389) % n) * 1000003LL - 123456789LL;
            fl[z] = pool[(i * 5) % 6];
            id[z] = i;
            if (i % 13 != 0)
                valid[z >> 3] |= static_cast<std::uint8_t>(1u << (z & 7));
        }
        DataFrame df;
        df.names = {"d", "t", "f", "id"};
        df.columns.push_back(
            Series::flat(TypeId::Date32, date.data(), n, valid.data()));
        df.columns.push_back(
            Series::flat(TypeId::Timestamp, ts.data(), n, valid.data()));
        df.columns.push_back(Series::flat(TypeId::Float32, fl.data(), n));
        df.columns.push_back(Series::flat_i64(id.data(), n));
        for (const char* key : {"d", "t", "f"}) {
            for (const bool desc : {false, true}) {
                INFO("key=" << key << " desc=" << desc);
                const DataFrame want = df.sort_by(key, desc);
                const DataFrame got = run(
                    df.lazy().memory_budget(2048).sort_by(key, desc).collect(
                        32));
                REQUIRE(got.num_rows() == n);
                const Series gi = got.column("id").materialize();
                const Series wi = want.column("id").materialize();
                for (std::int64_t i = 0; i < n; ++i)
                    REQUIRE(gi.data<std::int64_t>()[i] ==
                            wi.data<std::int64_t>()[i]);
            }
        }
        for (const std::int64_t rows : {std::int64_t{0}, std::int64_t{1}}) {
            DataFrame small;
            small.names = {"t", "id"};
            small.columns.push_back(
                Series::flat(TypeId::Timestamp, ts.data(), rows));
            small.columns.push_back(Series::flat_i64(id.data(), rows));
            const DataFrame got =
                run(small.lazy().memory_budget(1).sort_by("t").collect(8));
            CHECK(got.num_rows() == rows);
        }
    }

    TEST_CASE(
        "window functions under a tiny budget match the in-memory kernel") {
        // Chunks of about 1 KiB cut every partition several times, so each
        // function is answered across cuts: from carried state, context rows,
        // the figures collected while the sort read its input, or native
        // state. The reference is the same window over the whole input.
        const std::int64_t n = 2400;
        auto make = [&](std::int64_t parts) {
            std::vector<std::int64_t> p(n), o(n), v(n);
            std::vector<double> f(n), t(n);
            std::vector<std::int32_t> u(n);
            std::vector<std::uint8_t> vv((n + 7) / 8, 0), fv((n + 7) / 8, 0);
            for (std::int64_t i = 0; i < n; ++i) {
                const auto z = static_cast<std::size_t>(i);
                p[z] = (i * 7) % parts;
                o[z] = (i * 13) % 40;
                v[z] = (i * 17) % 53 - 20;
                f[z] = static_cast<double>((i * 11) % 97) * 0.25 - 9.0;
                t[z] = static_cast<double>((i * 5) % 31) * 1.5;
                u[z] = static_cast<std::int32_t>((i * 3) % 7);
                if (i % 7 != 0)
                    vv[z >> 3] |= static_cast<std::uint8_t>(1u << (z & 7));
                if (i % 11 != 0)
                    fv[z >> 3] |= static_cast<std::uint8_t>(1u << (z & 7));
            }
            DataFrame df;
            df.names = {"p", "o", "v", "f", "t", "u"};
            df.columns.push_back(Series::flat_i64(p.data(), n));
            df.columns.push_back(Series::flat_i64(o.data(), n));
            df.columns.push_back(Series::flat_i64(v.data(), n, vv.data()));
            df.columns.push_back(Series::flat_f64(f.data(), n, fv.data()));
            df.columns.push_back(Series::flat_f64(t.data(), n));
            df.columns.push_back(Series::flat(TypeId::Int32, u.data(), n));
            return df;
        };
        auto offset_spec = [](dftu_window_func func, const char* value,
                              const char* out, std::int64_t offset) {
            dftu_window_spec s{};
            s.func = func;
            s.value = value;
            s.out = out;
            s.param.offset = offset;
            return s;
        };
        auto frame_spec = [](dftu_window_func func, const char* value,
                             const char* out, std::int64_t pre,
                             std::int64_t fol, bool range = false,
                             std::int64_t min_count = 0, double q = 0.5,
                             const char* by = nullptr) {
            dftu_window_spec s{};
            s.func = func;
            s.value = value;
            s.out = out;
            s.param.frame = {
                min_count,
                pre,
                fol,
                range ? DFTU_WINDOW_FRAME_RANGE : DFTU_WINDOW_FRAME_ROWS,
                q,
                by};
            return s;
        };
        auto session_spec = [](const char* out, const char* time,
                               const char* end, double gap, double span) {
            dftu_window_spec s{};
            s.func = DFTU_WINDOW_SESSIONIZE;
            s.out = out;
            s.param.session = {time, end, gap, span};
            return s;
        };
        const std::int64_t ALL = DFTU_WINDOW_UNBOUNDED;
        using Specs = std::vector<dftu_window_spec>;
        const std::vector<std::pair<std::string, Specs>> groups = {
            {"ranks",
             {offset_spec(DFTU_WINDOW_NTILE, nullptr, "nt", 4),
              offset_spec(DFTU_WINDOW_ROW_NUMBER, nullptr, "rn", 0),
              offset_spec(DFTU_WINDOW_RANK, nullptr, "rk", 0),
              offset_spec(DFTU_WINDOW_DENSE_RANK, nullptr, "dr", 0),
              offset_spec(DFTU_WINDOW_PERCENT_RANK, nullptr, "pr", 0),
              offset_spec(DFTU_WINDOW_CUME_DIST, nullptr, "cd", 0)}},
            {"whole",
             {frame_spec(DFTU_WINDOW_FRAME_COUNT, "v", "wc", ALL, ALL),
              frame_spec(DFTU_WINDOW_FRAME_MIN, "u", "wmin", ALL, ALL),
              frame_spec(DFTU_WINDOW_FRAME_MAX, "v", "wmax", ALL, ALL),
              frame_spec(DFTU_WINDOW_FRAME_SUM, "v", "wsum", ALL, ALL),
              frame_spec(DFTU_WINDOW_FRAME_SUM, "u", "wusum", ALL, ALL, false,
                         3)}},
            {"sessions",
             {session_spec("s1", "t", nullptr, 3.0, 0.0),
              session_spec("s2", "t", "f", 1.0, 20.0)}},
            {"float frames",
             {frame_spec(DFTU_WINDOW_FRAME_SUM, "f", "fs", 2, 1),
              frame_spec(DFTU_WINDOW_FRAME_MEAN, "v", "vm", 3, 0),
              frame_spec(DFTU_WINDOW_FRAME_MEAN, "f", "fm", 1, 2),
              frame_spec(DFTU_WINDOW_FRAME_SUM, "f", "fs2", 5, 5, false, 3)}},
            {"row frames",
             {frame_spec(DFTU_WINDOW_FRAME_QUANTILE, "v", "q", 3, 1, false, 0,
                         0.3),
              frame_spec(DFTU_WINDOW_FRAME_COUNT_DISTINCT, "u", "cdist", 2, 2),
              frame_spec(DFTU_WINDOW_FRAME_ARG_MAX, "v", "amax", 2, 1, false, 0,
                         0.5, "f"),
              frame_spec(DFTU_WINDOW_FRAME_ARG_MIN, "v", "amin", 1, 1, false, 0,
                         0.5, "f"),
              frame_spec(DFTU_WINDOW_FRAME_MIN, "v", "fmin", 2, 2),
              frame_spec(DFTU_WINDOW_FRAME_MAX, "f", "fmax", 0, 3),
              frame_spec(DFTU_WINDOW_FRAME_COUNT, "v", "fcnt", 2, 0),
              frame_spec(DFTU_WINDOW_FRAME_SUM, "u", "fusum", 2, 2)}},
            {"range frames",
             {frame_spec(DFTU_WINDOW_FRAME_SUM, "v", "rs", 3, 1, true),
              frame_spec(DFTU_WINDOW_FRAME_MIN, "u", "rmin", 2, 2, true),
              frame_spec(DFTU_WINDOW_FRAME_COUNT, "v", "rc", 1, 0, true),
              frame_spec(DFTU_WINDOW_FRAME_QUANTILE, "v", "rq", 4, 2, true, 0,
                         0.7),
              frame_spec(DFTU_WINDOW_FRAME_COUNT_DISTINCT, "u", "rd", 5, 0,
                         true)}},
            {"neighbours",
             {offset_spec(DFTU_WINDOW_LAG, "v", "lag", 2),
              offset_spec(DFTU_WINDOW_LEAD, "f", "lead", 3),
              offset_spec(DFTU_WINDOW_DELTA, "v", "delta", 0)}},
        };
        const auto same = expect_frames_equal;
        struct Layout {
            std::int64_t parts;
            std::vector<std::string> by;
        };
        const std::vector<Layout> layouts = {{6, {"p"}}, {1, {"p"}}, {6, {}}};
        for (const Layout& layout : layouts) {
            const DataFrame df = make(layout.parts);
            for (const auto& [label, specs] : groups) {
                INFO("group=" << label << " parts=" << layout.parts
                              << " keys=" << layout.by.size());
                std::vector<const char*> pc;
                for (const std::string& s : layout.by) pc.push_back(s.c_str());
                const char* oc[] = {"o"};
                OpArgs args;
                args.strlist(1, pc.data(), static_cast<std::int32_t>(pc.size()))
                    .strlist(2, oc, 1)
                    .winlist(3, specs);
                std::vector<std::string> names = df.names;
                for (const dftu_window_spec& s : specs)
                    names.emplace_back(s.out);
                const LazyFrame plan =
                    df.lazy().frame_op("dftu.frame.window", args, {}, names);
                const DataFrame want = run(plan.collect(64));
                const DataFrame got = run(plan.memory_budget(4096).collect(64));
                same(got, want);
            }
        }
    }

    TEST_CASE(
        "window functions that need a whole partition stream under a tiny "
        "budget") {
        // Variance, frames from the partition start or to its end, float sums
        // over a range, collect frames and first, last and nth value used to
        // hold a whole partition. Each is compared bit for bit with the
        // window over the whole input; one partition must leave in several
        // morsels, or the path that holds it whole ran. The groups marked as
        // not streaming still hold a partition whole.
        const std::int64_t n = 3000;
        auto make = [&](std::int64_t parts) {
            std::vector<std::int64_t> p(n), o(n), v(n);
            std::vector<double> f(n), g(n);
            std::vector<float> h(n);
            std::vector<std::int32_t> u(n);
            std::vector<std::uint64_t> w(n);
            std::vector<std::string> s(n);
            std::vector<std::uint8_t> vv((n + 7) / 8, 0), fv((n + 7) / 8, 0),
                gv((n + 7) / 8, 0), wv((n + 7) / 8, 0);
            for (std::int64_t i = 0; i < n; ++i) {
                const auto z = static_cast<std::size_t>(i);
                p[z] = (i * 7) % parts;
                o[z] = (i * 13) % 40;
                v[z] = (i * 17) % 53 - 20;
                double x = static_cast<double>((i * 11) % 97) * 0.25 - 9.0;
                if (i % 53 == 0) x = 1e16;
                if (i % 59 == 0) x = 1e-3 * static_cast<double>(i);
                if (i % 97 == 5) x = std::nan("");
                if (i % 101 == 7) x = std::numeric_limits<double>::infinity();
                if (i % 103 == 9) x = -std::numeric_limits<double>::infinity();
                if (i % 107 == 11) x = -0.0;
                f[z] = x;
                g[z] = static_cast<double>((i * 13) % 40) * 0.5;
                h[z] = static_cast<float>((i * 19) % 83) * 0.125f - 4.0f;
                if (i % 23 == 4) g[z] = std::nan("");
                u[z] = static_cast<std::int32_t>((i * 3) % 7);
                w[z] = static_cast<std::uint64_t>((i * 29) % 1000);
                s[z] = "s" + std::to_string((i * 5) % 13);
                if (i % 7 != 0)
                    vv[z >> 3] |= static_cast<std::uint8_t>(1u << (z & 7));
                if (i % 11 != 0)
                    fv[z >> 3] |= static_cast<std::uint8_t>(1u << (z & 7));
                if (i % 17 != 3)
                    gv[z >> 3] |= static_cast<std::uint8_t>(1u << (z & 7));
                if (i % 5 != 0)
                    wv[z >> 3] |= static_cast<std::uint8_t>(1u << (z & 7));
            }
            DataFrame df;
            df.names = {"p", "o", "v", "f", "g", "u", "w", "s", "h"};
            df.columns.push_back(Series::flat_i64(p.data(), n));
            df.columns.push_back(Series::flat_i64(o.data(), n));
            df.columns.push_back(Series::flat_i64(v.data(), n, vv.data()));
            df.columns.push_back(Series::flat_f64(f.data(), n, fv.data()));
            df.columns.push_back(Series::flat_f64(g.data(), n, gv.data()));
            df.columns.push_back(Series::flat(TypeId::Int32, u.data(), n));
            df.columns.push_back(
                Series::flat(TypeId::Uint64, w.data(), n, wv.data()));
            df.columns.push_back(Series::strings(s));
            df.columns.push_back(Series::flat(TypeId::Float32, h.data(), n));
            return df;
        };
        auto offset_spec = [](dftu_window_func func, const char* value,
                              const char* out, std::int64_t offset) {
            dftu_window_spec s{};
            s.func = func;
            s.value = value;
            s.out = out;
            s.param.offset = offset;
            return s;
        };
        auto frame_spec = [](dftu_window_func func, const char* value,
                             const char* out, std::int64_t pre,
                             std::int64_t fol, bool range = false,
                             std::int64_t min_count = 0) {
            dftu_window_spec s{};
            s.func = func;
            s.value = value;
            s.out = out;
            s.param.frame = {
                min_count,
                pre,
                fol,
                range ? DFTU_WINDOW_FRAME_RANGE : DFTU_WINDOW_FRAME_ROWS,
                0.5,
                nullptr};
            return s;
        };
        const std::int64_t ALL = DFTU_WINDOW_UNBOUNDED;
        using Specs = std::vector<dftu_window_spec>;
        struct Group {
            std::string label;
            const char* order;
            bool streams;
            Specs specs;
        };
        const std::vector<Group> groups = {
            {"from the start, by rows",
             "o",
             true,
             {frame_spec(DFTU_WINDOW_FRAME_COUNT, "v", "c1", ALL, 0),
              frame_spec(DFTU_WINDOW_FRAME_COUNT, "s", "c2", ALL, 2),
              frame_spec(DFTU_WINDOW_FRAME_SUM, "v", "s1", ALL, 2),
              frame_spec(DFTU_WINDOW_FRAME_SUM, "u", "s2", ALL, 0, false, 3),
              frame_spec(DFTU_WINDOW_FRAME_SUM, "w", "s3", ALL, 1),
              frame_spec(DFTU_WINDOW_FRAME_SUM, "f", "s4", ALL, 1),
              frame_spec(DFTU_WINDOW_FRAME_MEAN, "f", "m1", ALL, 3),
              frame_spec(DFTU_WINDOW_FRAME_MEAN, "v", "m2", ALL, 0),
              frame_spec(DFTU_WINDOW_FRAME_MIN, "v", "n1", ALL, 1),
              frame_spec(DFTU_WINDOW_FRAME_MAX, "f", "x1", ALL, 2),
              frame_spec(DFTU_WINDOW_FRAME_MIN, "u", "n2", ALL, 0, false, 2),
              frame_spec(DFTU_WINDOW_FRAME_MAX, "w", "x2", ALL, 0),
              frame_spec(DFTU_WINDOW_FRAME_MAX, "h", "x3", ALL, 1),
              frame_spec(DFTU_WINDOW_FRAME_SUM, "h", "s5", ALL, 2),
              frame_spec(DFTU_WINDOW_FRAME_VAR, "h", "v4", 2, 1)}},
            {"from the start, by range",
             "g",
             true,
             {frame_spec(DFTU_WINDOW_FRAME_COUNT, "v", "rc", ALL, 1, true),
              frame_spec(DFTU_WINDOW_FRAME_SUM, "u", "rs", ALL, 2, true),
              frame_spec(DFTU_WINDOW_FRAME_MIN, "v", "rn", ALL, 0, true),
              frame_spec(DFTU_WINDOW_FRAME_MAX, "w", "rx", ALL, 3, true)}},
            {"float sums over a range",
             "g",
             true,
             {frame_spec(DFTU_WINDOW_FRAME_SUM, "f", "fs", 3, 1, true),
              frame_spec(DFTU_WINDOW_FRAME_MEAN, "v", "fm", 2, 2, true),
              frame_spec(DFTU_WINDOW_FRAME_SUM, "f", "fs2", 0, 0, true, 2)}},
            {"variance by rows",
             "o",
             true,
             {frame_spec(DFTU_WINDOW_FRAME_VAR, "v", "v1", 3, 1),
              frame_spec(DFTU_WINDOW_FRAME_STD, "f", "d1", 2, 2),
              frame_spec(DFTU_WINDOW_FRAME_VAR, "f", "v2", ALL, 2),
              frame_spec(DFTU_WINDOW_FRAME_STD, "u", "d2", 4, 0, false, 3),
              frame_spec(DFTU_WINDOW_FRAME_VAR, "w", "v3", 1, 1)}},
            {"variance by range",
             "g",
             true,
             {frame_spec(DFTU_WINDOW_FRAME_VAR, "f", "rv", 2, 1, true),
              frame_spec(DFTU_WINDOW_FRAME_STD, "v", "rd", 1, 3, true)}},
            {"collect",
             "o",
             true,
             {frame_spec(DFTU_WINDOW_FRAME_COLLECT, "v", "k1", 2, 1),
              frame_spec(DFTU_WINDOW_FRAME_COLLECT, "f", "k2", 1, 2),
              frame_spec(DFTU_WINDOW_FRAME_COLLECT, "s", "k3", 3, 0)}},
            {"collect by range",
             "g",
             true,
             {frame_spec(DFTU_WINDOW_FRAME_COLLECT, "v", "kr", 2, 1, true)}},
            {"first, last and nth value",
             "o",
             true,
             {offset_spec(DFTU_WINDOW_FIRST_VALUE, "v", "fv", 0),
              offset_spec(DFTU_WINDOW_FIRST_VALUE, "s", "fs", 0),
              offset_spec(DFTU_WINDOW_NTH_VALUE, "f", "n3", 3),
              offset_spec(DFTU_WINDOW_NTH_VALUE, "s", "n7", 7),
              offset_spec(DFTU_WINDOW_NTH_VALUE, "v", "n40", 40),
              offset_spec(DFTU_WINDOW_NTH_VALUE, "v", "nzero", 0),
              offset_spec(DFTU_WINDOW_LAST_VALUE, "v", "lv", 0),
              offset_spec(DFTU_WINDOW_LAST_VALUE, "s", "ls", 0),
              offset_spec(DFTU_WINDOW_LAST_VALUE, "f", "lf", 0)}},
            {"together",
             "o",
             true,
             {frame_spec(DFTU_WINDOW_FRAME_VAR, "f", "tv", 2, 1),
              frame_spec(DFTU_WINDOW_FRAME_SUM, "v", "ts", ALL, 1),
              offset_spec(DFTU_WINDOW_NTH_VALUE, "s", "tn", 4),
              offset_spec(DFTU_WINDOW_LAST_VALUE, "f", "tl", 0),
              offset_spec(DFTU_WINDOW_NTILE, nullptr, "tt", 5),
              offset_spec(DFTU_WINDOW_ROW_NUMBER, nullptr, "tr", 0)}},
            {"text extremes from the start",
             "o",
             true,
             {frame_spec(DFTU_WINDOW_FRAME_MIN, "s", "x1", ALL, 0),
              frame_spec(DFTU_WINDOW_FRAME_MAX, "s", "x2", ALL, 0, false, 3)}},
            {"whole variance",
             "o",
             true,
             {frame_spec(DFTU_WINDOW_FRAME_VAR, "f", "wv", ALL, ALL),
              frame_spec(DFTU_WINDOW_FRAME_STD, "v", "ws", ALL, ALL),
              frame_spec(DFTU_WINDOW_FRAME_VAR, "v", "wv3", ALL, ALL, false,
                         3)}},
            {"to the partition end",
             "o",
             true,
             {frame_spec(DFTU_WINDOW_FRAME_SUM, "v", "e1", 3, ALL),
              frame_spec(DFTU_WINDOW_FRAME_SUM, "f", "e2", 0, ALL),
              frame_spec(DFTU_WINDOW_FRAME_MEAN, "f", "e3", 5, ALL),
              frame_spec(DFTU_WINDOW_FRAME_COUNT, "f", "e4", 2, ALL),
              frame_spec(DFTU_WINDOW_FRAME_MEAN, "v", "e5", 1, ALL)}},
            {"whole float sums and means",
             "o",
             true,
             {frame_spec(DFTU_WINDOW_FRAME_SUM, "f", "wf", ALL, ALL),
              frame_spec(DFTU_WINDOW_FRAME_MEAN, "f", "wm", ALL, ALL),
              frame_spec(DFTU_WINDOW_FRAME_MEAN, "v", "wmv", ALL, ALL)}},
            {"min and max to the partition end",
             "o",
             true,
             {frame_spec(DFTU_WINDOW_FRAME_MIN, "v", "z1", 3, ALL),
              frame_spec(DFTU_WINDOW_FRAME_MAX, "f", "z2", 0, ALL),
              frame_spec(DFTU_WINDOW_FRAME_MIN, "f", "z3", 2, ALL, false, 3),
              frame_spec(DFTU_WINDOW_FRAME_MAX, "s", "z4", 4, ALL),
              frame_spec(DFTU_WINDOW_FRAME_MIN, "s", "z5", 0, ALL, false, 2),
              frame_spec(DFTU_WINDOW_FRAME_MIN, "w", "z6", 1, ALL),
              frame_spec(DFTU_WINDOW_FRAME_MAX, "h", "z7", 5, ALL),
              frame_spec(DFTU_WINDOW_FRAME_MIN, "u", "z8", 2, ALL, false, 2),
              frame_spec(DFTU_WINDOW_FRAME_MAX, "g", "z9", 40, ALL)}},
            {"variance to the partition end",
             "o",
             true,
             {frame_spec(DFTU_WINDOW_FRAME_VAR, "v", "y1", 3, ALL),
              frame_spec(DFTU_WINDOW_FRAME_STD, "f", "y2", 0, ALL),
              frame_spec(DFTU_WINDOW_FRAME_VAR, "u", "y3", 2, ALL, false, 3),
              frame_spec(DFTU_WINDOW_FRAME_STD, "w", "y4", 5, ALL),
              frame_spec(DFTU_WINDOW_FRAME_VAR, "h", "y5", 1, ALL),
              frame_spec(DFTU_WINDOW_FRAME_VAR, "g", "y6", 200, ALL)}},
            {"text extremes reading ahead",
             "o",
             true,
             {frame_spec(DFTU_WINDOW_FRAME_MIN, "s", "a1", ALL, 3),
              frame_spec(DFTU_WINDOW_FRAME_MAX, "s", "a2", ALL, 1, false, 2),
              frame_spec(DFTU_WINDOW_FRAME_MAX, "s", "a3", ALL, 0)}},
            {"range frames to the partition end",
             "g",
             true,
             {frame_spec(DFTU_WINDOW_FRAME_COUNT, "v", "t1", 1, ALL, true),
              frame_spec(DFTU_WINDOW_FRAME_SUM, "v", "t2", 2, ALL, true),
              frame_spec(DFTU_WINDOW_FRAME_SUM, "u", "t3", 0, ALL, true, 3),
              frame_spec(DFTU_WINDOW_FRAME_MEAN, "v", "t4", 3, ALL, true),
              frame_spec(DFTU_WINDOW_FRAME_SUM, "f", "t5", 2, ALL, true),
              frame_spec(DFTU_WINDOW_FRAME_MEAN, "f", "t6", 1, ALL, true),
              frame_spec(DFTU_WINDOW_FRAME_SUM, "h", "t7", 0, ALL, true),
              frame_spec(DFTU_WINDOW_FRAME_SUM, "w", "t8", 4, ALL, true),
              frame_spec(DFTU_WINDOW_FRAME_COUNT, "s", "t9", 2, ALL, true)}},
            {"range frames to the end, ordered by an integer",
             "o",
             true,
             {frame_spec(DFTU_WINDOW_FRAME_SUM, "f", "q1", 3, ALL, true),
              frame_spec(DFTU_WINDOW_FRAME_COUNT, "v", "q2", 0, ALL, true),
              frame_spec(DFTU_WINDOW_FRAME_MEAN, "w", "q3", 5, ALL, true)}},
            {"range and row frames together",
             "g",
             true,
             {frame_spec(DFTU_WINDOW_FRAME_COUNT, "v", "m1", 2, 0, true),
              frame_spec(DFTU_WINDOW_FRAME_SUM, "u", "m2", 2, 1),
              frame_spec(DFTU_WINDOW_FRAME_MIN, "w", "m3", 1, 2),
              frame_spec(DFTU_WINDOW_FRAME_SUM, "f", "m4", 2, 1),
              frame_spec(DFTU_WINDOW_FRAME_MAX, "v", "m5", 1, 1, true),
              offset_spec(DFTU_WINDOW_LAG, "v", "m6", 3)}},
            {"still whole",
             "g",
             false,
             {frame_spec(DFTU_WINDOW_FRAME_SUM, "f", "w1", ALL, 2, true),
              frame_spec(DFTU_WINDOW_FRAME_MIN, "v", "w2", 2, ALL, true),
              frame_spec(DFTU_WINDOW_FRAME_VAR, "v", "w3", 1, ALL, true),
              frame_spec(DFTU_WINDOW_FRAME_QUANTILE, "v", "w4", ALL, 1),
              frame_spec(DFTU_WINDOW_FRAME_COLLECT, "v", "w5", ALL, 0),
              offset_spec(DFTU_WINDOW_NTH_VALUE, "v", "w6", 100000)}},
            {"running functions with a look-around",
             "o",
             false,
             {offset_spec(DFTU_WINDOW_RUNNING_SUM, "v", "r1", 0),
              offset_spec(DFTU_WINDOW_LAG, "f", "r2", 2)}},
            {"two running functions on one column",
             "o",
             false,
             {offset_spec(DFTU_WINDOW_RUNNING_SUM, "v", "r3", 0),
              offset_spec(DFTU_WINDOW_RUNNING_MIN, "v", "r4", 0)}},
        };
        struct Layout {
            std::int64_t parts;
            std::vector<std::string> by;
        };
        const std::vector<Layout> layouts = {{7, {"p"}}, {1, {"p"}}, {7, {}}};
        for (const Layout& layout : layouts) {
            const DataFrame df = make(layout.parts);
            for (const Group& group : groups) {
                INFO("group=" << group.label << " parts=" << layout.parts
                              << " keys=" << layout.by.size());
                std::vector<const char*> pc;
                for (const std::string& s : layout.by) pc.push_back(s.c_str());
                const char* oc[] = {group.order};
                OpArgs args;
                args.strlist(1, pc.data(), static_cast<std::int32_t>(pc.size()))
                    .strlist(2, oc, 1)
                    .winlist(3, group.specs);
                std::vector<std::string> names = df.names;
                for (const dftu_window_spec& s : group.specs)
                    names.emplace_back(s.out);
                const LazyFrame plan =
                    df.lazy().frame_op("dftu.frame.window", args, {}, names);
                const DataFrame want = run(plan.collect(64));
                auto [chunk_rows, got] =
                    run_stream_probe(plan.memory_budget(4096).stream(64));
                expect_frames_equal(got, want);
                if (group.streams && layout.parts == 1 && !layout.by.empty()) {
                    // One partition of n rows: held whole it leaves as one
                    // morsel.
                    REQUIRE(chunk_rows.size() > 1);
                    CHECK(*std::max_element(chunk_rows.begin(),
                                            chunk_rows.end()) < n / 4);
                }
            }
        }
    }

    TEST_CASE("a window with more partitions than the figures table holds") {
        // Half the rows are one partition and the rest are one row each, so
        // the table of figures overflows and the big partition must still be
        // cut.
        const std::int64_t n = 30000;
        std::vector<std::int64_t> p(n), o(n), v(n);
        std::vector<std::string> s(n);
        for (std::int64_t i = 0; i < n; ++i) {
            const auto z = static_cast<std::size_t>(i);
            p[z] = i % 2 == 0 ? 0 : i;
            o[z] = (i * 13) % 977;
            v[z] = (i * 17) % 53 - 20;
            s[z] = "s" + std::to_string(i % 31);
        }
        DataFrame df;
        df.names = {"p", "o", "v", "s"};
        df.columns.push_back(Series::flat_i64(p.data(), n));
        df.columns.push_back(Series::flat_i64(o.data(), n));
        df.columns.push_back(Series::flat_i64(v.data(), n));
        df.columns.push_back(Series::strings(s));
        auto spec = [](dftu_window_func func, const char* value,
                       const char* out, std::int64_t offset) {
            dftu_window_spec w{};
            w.func = func;
            w.value = value;
            w.out = out;
            w.param.offset = offset;
            return w;
        };
        dftu_window_spec whole{};
        whole.func = DFTU_WINDOW_FRAME_SUM;
        whole.value = "v";
        whole.out = "total";
        whole.param.frame = {0,
                             DFTU_WINDOW_UNBOUNDED,
                             DFTU_WINDOW_UNBOUNDED,
                             DFTU_WINDOW_FRAME_ROWS,
                             0.5,
                             nullptr};
        const std::vector<dftu_window_spec> specs = {
            spec(DFTU_WINDOW_NTILE, nullptr, "nt", 4),
            spec(DFTU_WINDOW_PERCENT_RANK, nullptr, "pr", 0),
            spec(DFTU_WINDOW_CUME_DIST, nullptr, "cd", 0),
            spec(DFTU_WINDOW_ROW_NUMBER, nullptr, "rn", 0), whole};
        const char* pc[] = {"p"};
        const char* oc[] = {"o"};
        OpArgs args;
        args.strlist(1, pc, 1).strlist(2, oc, 1).winlist(3, specs);
        std::vector<std::string> names = df.names;
        for (const dftu_window_spec& w : specs) names.emplace_back(w.out);
        const LazyFrame plan =
            df.lazy().frame_op("dftu.frame.window", args, {}, names);
        const DataFrame want = run(plan.collect(64));
        auto [chunk_rows, got] =
            run_stream_probe(plan.memory_budget(4096).stream(64));
        expect_frames_equal(got, want);
        REQUIRE(chunk_rows.size() > 1);
        CHECK(*std::max_element(chunk_rows.begin(), chunk_rows.end()) < n / 4);
    }

    TEST_CASE("native group transforms match a row-by-row reference") {
        const std::int64_t n = 4000;
        std::vector<std::int64_t> k(n), v(n);
        std::vector<double> f(n);
        std::vector<std::int32_t> u(n);
        std::vector<std::uint64_t> w(n);
        std::vector<std::uint8_t> vv((n + 7) / 8, 0), fv((n + 7) / 8, 0),
            wv((n + 7) / 8, 0);
        for (std::int64_t i = 0; i < n; ++i) {
            const auto z = static_cast<std::size_t>(i);
            k[z] = (i * 31) % 29;
            v[z] = (i * 17) % 101 - 40;
            f[z] = static_cast<double>((i * 7) % 53) * 0.5 - 12.0;
            u[z] = static_cast<std::int32_t>((i * 13) % 59) - 20;
            w[z] = static_cast<std::uint64_t>((i * 11) % 97);
            if (i % 7 != 0)
                vv[z >> 3] |= static_cast<std::uint8_t>(1u << (z & 7));
            if (i % 11 != 0)
                fv[z >> 3] |= static_cast<std::uint8_t>(1u << (z & 7));
            if (i % 5 != 0)
                wv[z >> 3] |= static_cast<std::uint8_t>(1u << (z & 7));
        }
        DataFrame df;
        df.names = {"k", "v", "f", "u", "w"};
        df.columns.push_back(Series::flat_i64(k.data(), n));
        df.columns.push_back(Series::flat_i64(v.data(), n, vv.data()));
        df.columns.push_back(Series::flat_f64(f.data(), n, fv.data()));
        df.columns.push_back(Series::flat(TypeId::Int32, u.data(), n));
        df.columns.push_back(
            Series::flat(TypeId::Uint64, w.data(), n, wv.data()));

        using Cell = std::optional<double>;
        struct Col {
            std::string name;
            std::vector<Cell> cells;
        };
        std::vector<Col> cols(4);
        cols[0] = {"v", {}};
        cols[1] = {"f", {}};
        cols[2] = {"u", {}};
        cols[3] = {"w", {}};
        for (std::int64_t i = 0; i < n; ++i) {
            const auto z = static_cast<std::size_t>(i);
            cols[0].cells.push_back(i % 7 != 0 ? Cell(static_cast<double>(v[z]))
                                               : Cell());
            cols[1].cells.push_back(i % 11 != 0 ? Cell(f[z]) : Cell());
            cols[2].cells.push_back(Cell(static_cast<double>(u[z])));
            cols[3].cells.push_back(i % 5 != 0 ? Cell(static_cast<double>(w[z]))
                                               : Cell());
        }
        auto read = [](const Series& s0, std::int64_t i) -> Cell {
            const Series s = s0.materialize();
            if (s.is_null(i)) return Cell();
            switch (s.type()) {
                case TypeId::Int64:
                    return static_cast<double>(s.data<std::int64_t>()[i]);
                case TypeId::Int32:
                    return static_cast<double>(s.data<std::int32_t>()[i]);
                case TypeId::Uint64:
                    return static_cast<double>(s.data<std::uint64_t>()[i]);
                case TypeId::Float64:
                    return s.data<double>()[i];
                default:
                    throw std::runtime_error("unexpected result type");
            }
        };
        // The reference: walk each group's rows in input order.
        auto reference = [&](GroupwiseOp op, std::int64_t shift, bool keyed,
                             const Col& col) {
            std::vector<Cell> want(static_cast<std::size_t>(n));
            std::map<std::int64_t, std::vector<std::int64_t>> rows;
            for (std::int64_t i = 0; i < n; ++i)
                rows[keyed ? k[static_cast<std::size_t>(i)] : 0].push_back(i);
            for (const auto& [key, idx] : rows) {
                double sum = 0.0, prod = 1.0;
                Cell ext, last_present;
                for (std::size_t j = 0; j < idx.size(); ++j) {
                    const std::int64_t i = idx[j];
                    const Cell& c = col.cells[static_cast<std::size_t>(i)];
                    Cell out;
                    switch (op) {
                        case GroupwiseOp::CumSum:
                            if (c) {
                                sum += *c;
                                out = sum + (*c - *c);
                            }
                            break;
                        case GroupwiseOp::CumProd:
                            if (c) {
                                prod *= *c;
                                out = prod + (*c - *c);
                            }
                            break;
                        case GroupwiseOp::CumMax:
                            if (c) {
                                if (!ext || *c > *ext) ext = c;
                                out = *ext + (*c - *c);
                            }
                            break;
                        case GroupwiseOp::CumMin:
                            if (c) {
                                if (!ext || *c < *ext) ext = c;
                                out = *ext + (*c - *c);
                            }
                            break;
                        case GroupwiseOp::Shift:
                            if (static_cast<std::int64_t>(j) >= shift)
                                out = col.cells[static_cast<std::size_t>(
                                    idx[j - static_cast<std::size_t>(shift)])];
                            break;
                        case GroupwiseOp::Diff:
                            if (j > 0 && c) {
                                const Cell& p =
                                    col.cells[static_cast<std::size_t>(
                                        idx[j - 1])];
                                if (p) out = *c - *p;
                            }
                            break;
                        case GroupwiseOp::FFill:
                            if (c) last_present = c;
                            out = last_present;
                            break;
                        default:
                            break;
                    }
                    want[static_cast<std::size_t>(i)] = out;
                }
            }
            return want;
        };
        struct Case {
            GroupwiseOp op;
            std::int64_t shift;
        };
        const std::vector<Case> cases = {
            {GroupwiseOp::CumSum, 0}, {GroupwiseOp::CumProd, 0},
            {GroupwiseOp::CumMax, 0}, {GroupwiseOp::CumMin, 0},
            {GroupwiseOp::Shift, 0},  {GroupwiseOp::Shift, 1},
            {GroupwiseOp::Shift, 4},  {GroupwiseOp::Diff, 0},
            {GroupwiseOp::FFill, 0},
        };
        for (const bool keyed : {true, false}) {
            for (const std::uint64_t budget :
                 {std::uint64_t{0}, std::uint64_t{4096}}) {
                for (const Case& c : cases) {
                    INFO("op=" << static_cast<int>(c.op) << " shift=" << c.shift
                               << " keyed=" << keyed << " budget=" << budget);
                    LazyFrame lf = df.lazy();
                    if (budget) lf = lf.memory_budget(budget);
                    const std::vector<std::string> keys =
                        keyed ? std::vector<std::string>{"k"}
                              : std::vector<std::string>{};
                    DataFrame got = run(
                        lf.group_by(keys)
                            .transform(
                                c.op, c.shift,
                                dftracer::utils::dataframe::RankMethod::Average,
                                true)
                            .collect(64));
                    for (const Col& col : cols) {
                        const auto at = std::find(got.names.begin(),
                                                  got.names.end(), col.name);
                        if (at == got.names.end()) continue;
                        const Series& out =
                            got.columns[static_cast<std::size_t>(
                                at - got.names.begin())];
                        REQUIRE(out.length() == n);
                        const std::vector<Cell> want =
                            reference(c.op, c.shift, keyed, col);
                        for (std::int64_t i = 0; i < n; ++i) {
                            const Cell have = read(out, i);
                            const Cell& expect =
                                want[static_cast<std::size_t>(i)];
                            REQUIRE(have.has_value() == expect.has_value());
                            if (have) REQUIRE(*have == *expect);
                        }
                    }
                }
                {
                    LazyFrame lf = df.lazy();
                    if (budget) lf = lf.memory_budget(budget);
                    const std::vector<std::string> keys =
                        keyed ? std::vector<std::string>{"k"}
                              : std::vector<std::string>{};
                    DataFrame got = run(
                        lf.group_by(keys)
                            .transform(
                                GroupwiseOp::CumCount, 0,
                                dftracer::utils::dataframe::RankMethod::Average,
                                true)
                            .collect(64));
                    REQUIRE(got.names == std::vector<std::string>{"cumcount"});
                    const Series cc = got.columns[0].materialize();
                    std::map<std::int64_t, std::int64_t> seen;
                    for (std::int64_t i = 0; i < n; ++i)
                        REQUIRE(
                            cc.data<std::int64_t>()[i] ==
                            seen[keyed ? k[static_cast<std::size_t>(i)] : 0]++);
                }
            }
        }
    }

    TEST_CASE("native group transforms equal the composed window plan") {
        using dftracer::utils::dataframe::composed_group_transform;
        using dftracer::utils::dataframe::RankMethod;
        const std::int64_t n = 300;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        std::vector<std::int64_t> k(n);
        std::vector<std::int32_t> k2(n);
        std::vector<std::string> names(n);
        std::vector<std::uint8_t> kv((n + 7) / 8, 0), vv((n + 7) / 8, 0);
        std::vector<std::int64_t> base(n);
        std::vector<double> fbase(n);
        for (std::int64_t i = 0; i < n; ++i) {
            const auto z = static_cast<std::size_t>(i);
            k[z] = (i * 5) % 7;
            k2[z] = static_cast<std::int32_t>(i % 3);
            names[z] = std::string(1, static_cast<char>('a' + i % 4)) +
                       (i % 5 == 0 ? "-long-key-over-the-sso-limit" : "");
            if (i % 23 != 0)
                kv[z >> 3] |= static_cast<std::uint8_t>(1u << (z & 7));
            if (i % 9 != 0)
                vv[z >> 3] |= static_cast<std::uint8_t>(1u << (z & 7));
            base[z] = (i * 37) % 61;
            fbase[z] = static_cast<double>(base[z]) * 0.5 - 7.25;
            if (i % 31 == 5) fbase[z] = nan;
            if (i % 31 == 11) fbase[z] = inf;
            if (i % 31 == 17) fbase[z] = -0.0;
        }
        auto typed = [&](TypeId t) {
            auto as = [&](auto tag, bool floating) {
                using T = decltype(tag);
                std::vector<T> v(static_cast<std::size_t>(n));
                for (std::size_t i = 0; i < v.size(); ++i)
                    v[i] = floating ? static_cast<T>(fbase[i])
                                    : static_cast<T>(base[i]);
                return Series::flat(t, v.data(), n, vv.data());
            };
            switch (t) {
                case TypeId::Int8:
                    return as(std::int8_t{}, false);
                case TypeId::Int16:
                    return as(std::int16_t{}, false);
                case TypeId::Int32:
                    return as(std::int32_t{}, false);
                case TypeId::Int64:
                    return as(std::int64_t{}, false);
                case TypeId::Uint8:
                    return as(std::uint8_t{}, false);
                case TypeId::Uint16:
                    return as(std::uint16_t{}, false);
                case TypeId::Uint32:
                    return as(std::uint32_t{}, false);
                case TypeId::Uint64:
                    return as(std::uint64_t{}, false);
                case TypeId::Float32:
                    return as(float{}, true);
                default:
                    return as(double{}, true);
            }
        };
        struct Case {
            GroupwiseOp op;
            std::int64_t n;
        };
        const std::vector<Case> cases = {
            {GroupwiseOp::CumSum, 0},   {GroupwiseOp::CumMax, 0},
            {GroupwiseOp::CumMin, 0},   {GroupwiseOp::CumProd, 0},
            {GroupwiseOp::CumCount, 0}, {GroupwiseOp::Shift, 1},
            {GroupwiseOp::Shift, 3},    {GroupwiseOp::Diff, 0},
            {GroupwiseOp::FFill, 0}};
        const std::vector<TypeId> types = {
            TypeId::Int8,    TypeId::Int16,  TypeId::Int32,  TypeId::Int64,
            TypeId::Uint8,   TypeId::Uint16, TypeId::Uint32, TypeId::Uint64,
            TypeId::Float32, TypeId::Float64};
        for (const TypeId t : types) {
            DataFrame num;
            num.names = {"k", "v", "k2"};
            num.columns.push_back(Series::flat_i64(k.data(), n, kv.data()));
            num.columns.push_back(typed(t));
            num.columns.push_back(Series::flat(TypeId::Int32, k2.data(), n));
            DataFrame str;
            str.names = {"s", "v"};
            str.columns.push_back(Series::strings(names));
            str.columns.push_back(typed(t));
            struct Plan {
                const DataFrame* df;
                std::vector<std::string> keys;
            };
            const std::vector<Plan> plans = {
                {&num, {"k"}}, {&num, {}}, {&num, {"k", "k2"}}, {&str, {"s"}}};
            for (const Plan& plan : plans) {
                const DataFrame& df = *plan.df;
                const std::vector<std::string>& keys = plan.keys;
                for (const Case& c : cases) {
                    INFO("type=" << static_cast<int>(t)
                                 << " op=" << static_cast<int>(c.op)
                                 << " n=" << c.n << " keys=" << keys.size());
                    const LazyFrame fast = df.lazy().group_by(keys).transform(
                        c.op, c.n, RankMethod::Average, true);
                    REQUIRE(fast.explain().find("group_transform") !=
                            std::string::npos);
                    const DataFrame want =
                        run(composed_group_transform(df.lazy(), keys, c.op, c.n,
                                                     RankMethod::Average, true)
                                .collect(64));
                    for (const std::uint64_t budget :
                         {std::uint64_t{0}, std::uint64_t{4096}}) {
                        const DataFrame got =
                            run(fast.memory_budget(budget).collect(64));
                        expect_frames_equal(got, want);
                    }
                }
            }
        }
    }

    TEST_CASE("head_by with many hash leaves equals the in-memory result") {
        const std::int64_t n = 60000;
        std::vector<std::int64_t> k(n), v(n);
        for (std::int64_t i = 0; i < n; ++i) {
            k[static_cast<std::size_t>(i)] = (i * 7919) % 15000;
            v[static_cast<std::size_t>(i)] = i;
        }
        DataFrame df;
        df.names = {"k", "v"};
        df.columns.push_back(Series::flat_i64(k.data(), n));
        df.columns.push_back(Series::flat_i64(v.data(), n));
        const DataFrame want = run(
            df.lazy().head_by(std::vector<std::string>{"k"}, 2).collect(4096));
        const DataFrame got = run(df.lazy()
                                      .memory_budget(32768)
                                      .head_by(std::vector<std::string>{"k"}, 2)
                                      .collect(4096));
        REQUIRE(got.num_rows() == want.num_rows());
        const Series a = got.columns[1].materialize();
        const Series b = want.columns[1].materialize();
        for (std::int64_t r = 0; r < a.length(); ++r)
            REQUIRE(a.data<std::int64_t>()[r] == b.data<std::int64_t>()[r]);
    }

    TEST_CASE("native group transforms: empty, one row, float keys, overflow") {
        using dftracer::utils::dataframe::composed_group_transform;
        using dftracer::utils::dataframe::RankMethod;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        auto frame = [&](std::vector<double> k, std::vector<std::int64_t> v) {
            const auto n = static_cast<std::int64_t>(k.size());
            DataFrame df;
            df.names = {"k", "v"};
            df.columns.push_back(Series::flat_f64(k.data(), n));
            df.columns.push_back(Series::flat_i64(v.data(), n));
            return df;
        };
        const std::vector<std::string> keys = {"k"};
        const std::vector<GroupwiseOp> ops = {
            GroupwiseOp::CumSum,   GroupwiseOp::CumMax, GroupwiseOp::CumMin,
            GroupwiseOp::CumCount, GroupwiseOp::Diff,   GroupwiseOp::FFill};
        std::vector<DataFrame> frames;
        frames.push_back(frame({}, {}));
        frames.push_back(frame({1.5}, {7}));
        frames.push_back(
            frame({0.0, -0.0, nan, nan, 1.0, 0.0}, {1, 2, 3, 4, 5, 6}));
        for (const DataFrame& df : frames) {
            for (const GroupwiseOp op : ops) {
                INFO("rows=" << df.num_rows()
                             << " op=" << static_cast<int>(op));
                const LazyFrame fast = df.lazy().group_by(keys).transform(
                    op, 0, RankMethod::Average, true);
                REQUIRE(fast.explain().find("group_transform") !=
                        std::string::npos);
                expect_frames_equal(
                    run(fast.collect(64)),
                    run(composed_group_transform(df.lazy(), keys, op, 0,
                                                 RankMethod::Average, true)
                            .collect(64)));
            }
        }
        const std::int64_t big = std::numeric_limits<std::int64_t>::max();
        const DataFrame wide = frame({1.0, 1.0}, {big, 1});
        CHECK_THROWS_AS(run(wide.lazy()
                                .group_by(keys)
                                .transform(GroupwiseOp::CumSum, 0,
                                           RankMethod::Average, true)
                                .collect(64)),
                        std::overflow_error);
        // The window plan reports the same overflow, but through the op ABI,
        // which drops the message.
        CHECK_THROWS(
            run(composed_group_transform(wide.lazy(), keys, GroupwiseOp::CumSum,
                                         0, RankMethod::Average, true)
                    .collect(64)));
        // A transform the native step does not cover is composed.
        CHECK(frames[2]
                  .lazy()
                  .group_by(keys)
                  .transform(GroupwiseOp::Rank, 0, RankMethod::Min, true)
                  .explain()
                  .find("group_transform") == std::string::npos);
    }

    TEST_CASE(
        "window op and native transform fail with the same overflow text") {
        using dftracer::utils::dataframe::RankMethod;
        using dftracer::utils::dataframe::WindowColumn;
        using dftracer::utils::dataframe::WindowFunc;
        const std::int64_t big = std::numeric_limits<std::int64_t>::max();
        const std::int64_t small = std::numeric_limits<std::int64_t>::min();
        const std::uint64_t ubig = std::numeric_limits<std::uint64_t>::max();
        const std::vector<std::int64_t> k = {1, 1}, ord = {0, 1};
        struct Case {
            const char* name;
            DataFrame df;
            WindowFunc func;
            GroupwiseOp op;
            const char* text;
        };
        auto frame = [&](TypeId t, const void* v) {
            DataFrame df;
            df.names = {"k", "ord", "v"};
            df.columns.push_back(Series::flat_i64(k.data(), 2));
            df.columns.push_back(Series::flat_i64(ord.data(), 2));
            df.columns.push_back(Series::flat(t, v, 2));
            return df;
        };
        const std::vector<std::int64_t> sum_i = {big, 1};
        const std::vector<std::uint64_t> sum_u = {ubig, 1};
        const std::vector<std::int64_t> diff_i = {1, small};
        std::vector<Case> cases;
        cases.push_back({"cumsum int64", frame(TypeId::Int64, sum_i.data()),
                         WindowFunc::RunningSum, GroupwiseOp::CumSum,
                         "window: RUNNING_SUM overflows int64"});
        cases.push_back({"cumsum uint64", frame(TypeId::Uint64, sum_u.data()),
                         WindowFunc::RunningSum, GroupwiseOp::CumSum,
                         "window: RUNNING_SUM overflows uint64"});
        cases.push_back({"diff int64", frame(TypeId::Int64, diff_i.data()),
                         WindowFunc::Delta, GroupwiseOp::Diff,
                         "window: DELTA overflows int64"});
        for (const Case& c : cases) {
            INFO(c.name);
            WindowColumn w{};
            w.func = c.func;
            w.out = "out";
            w.set_value("v");
            CHECK_THROWS_WITH_AS(
                dftracer::utils::dataframe::window(c.df, {"k"}, {"ord"}, {w}),
                c.text, std::overflow_error);
            const LazyFrame fast =
                c.df.lazy()
                    .select({"k", "v"})
                    .group_by(std::vector<std::string>{"k"})
                    .transform(c.op, 0, RankMethod::Average, true);
            REQUIRE(fast.explain().find("group_transform") !=
                    std::string::npos);
            CHECK_THROWS_WITH_AS(run(fast.collect(64)), c.text,
                                 std::overflow_error);
        }
    }

    TEST_CASE(
        "a spilled group transform groups one numeric key without strings") {
        using dftracer::utils::dataframe::composed_group_transform;
        using dftracer::utils::dataframe::native_transform_single_key_rows;
        using dftracer::utils::dataframe::RankMethod;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        const std::int64_t n = 2000;
        std::vector<std::int64_t> k(n), v(n);
        std::vector<double> kf(n);
        std::vector<std::uint8_t> kv((n + 7) / 8, 0);
        std::vector<std::string> names(n);
        const double float_keys[] = {0.0, -0.0, nan, inf, -inf, 1.5, -1.5, 2.5};
        for (std::int64_t i = 0; i < n; ++i) {
            const auto z = static_cast<std::size_t>(i);
            k[z] = (i * 7) % 37;
            kf[z] = float_keys[(i * 5) % 8];
            v[z] = i % 101 - 50;
            names[z] = "g" + std::to_string(k[z]);
            if (i % 19 != 0)
                kv[z >> 3] |= static_cast<std::uint8_t>(1u << (z & 7));
        }
        DataFrame num;
        num.names = {"k", "v"};
        num.columns.push_back(Series::flat_i64(k.data(), n, kv.data()));
        num.columns.push_back(Series::flat_i64(v.data(), n));
        // Float keys: negative zero and zero are one group, every NaN is one.
        DataFrame flt;
        flt.names = {"k", "v"};
        flt.columns.push_back(Series::flat_f64(kf.data(), n, kv.data()));
        flt.columns.push_back(Series::flat_i64(v.data(), n));
        DataFrame str;
        str.names = {"s", "v"};
        str.columns.push_back(Series::strings(names));
        str.columns.push_back(Series::flat_i64(v.data(), n));
        const std::vector<std::string> by_k = {"k"};
        const std::vector<std::string> by_s = {"s"};
        for (const DataFrame* keyed : {&num, &flt}) {
            for (const GroupwiseOp op :
                 {GroupwiseOp::CumSum, GroupwiseOp::Diff, GroupwiseOp::Shift}) {
                INFO("float key=" << (keyed == &flt)
                                  << " op=" << static_cast<int>(op));
                const LazyFrame fast = keyed->lazy().group_by(by_k).transform(
                    op, 1, RankMethod::Average, true);
                REQUIRE(fast.explain().find("group_transform") !=
                        std::string::npos);
                // The default budget keeps it in memory: a counting pass over
                // every row for the group count, then the hash pass over every
                // row.
                const std::uint64_t first = native_transform_single_key_rows();
                run(fast.collect(64));
                CHECK(native_transform_single_key_rows() - first ==
                      static_cast<std::uint64_t>(2 * n));
                // This budget spools the input after a few morsels, and the
                // sorted pass then reads every row.
                const std::uint64_t before = native_transform_single_key_rows();
                const DataFrame got = run(fast.memory_budget(4096).collect(64));
                CHECK(native_transform_single_key_rows() - before >=
                      static_cast<std::uint64_t>(n));
                expect_frames_equal(got, run(composed_group_transform(
                                                 keyed->lazy(), by_k, op, 1,
                                                 RankMethod::Average, true)
                                                 .collect(64)));
            }
        }
        // A string key takes the other path and does not count.
        const std::uint64_t mid = native_transform_single_key_rows();
        run(str.lazy()
                .group_by(by_s)
                .transform(GroupwiseOp::CumSum, 1, RankMethod::Average, true)
                .memory_budget(4096)
                .collect(64));
        CHECK(native_transform_single_key_rows() == mid);
    }

    TEST_CASE("group transforms order NaN last and group -0.0 with 0.0") {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const std::vector<double> k = {0.0, -0.0, 0.0, nan, nan, 1.0, 1.0};
        const std::vector<double> v = {1.0, nan, 5.0, 2.0, 7.0, 3.0, 4.0};
        const std::vector<double> w = {5.0, nan, 1.0, 2.0, 7.0, 3.0, 4.0};
        const auto n = static_cast<std::int64_t>(k.size());
        DataFrame df;
        df.names = {"k", "v", "w"};
        df.columns.push_back(Series::flat_f64(k.data(), n));
        df.columns.push_back(Series::flat_f64(v.data(), n));
        df.columns.push_back(Series::flat_f64(w.data(), n));
        const auto column = [](const DataFrame& f, const char* name) {
            const auto at =
                std::find(f.names.begin(), f.names.end(), std::string(name));
            REQUIRE(at != f.names.end());
            const Series s =
                f.columns[static_cast<std::size_t>(at - f.names.begin())]
                    .materialize();
            return std::vector<double>(s.data<double>(),
                                       s.data<double>() + s.length());
        };
        for (const std::uint64_t budget :
             {std::uint64_t{0}, std::uint64_t{4096}}) {
            INFO("budget=" << budget);
            const DataFrame mx = run(
                df.lazy()
                    .memory_budget(budget)
                    .group_by(std::vector<std::string>{"k"})
                    .transform(GroupwiseOp::CumMax, 0,
                               dftracer::utils::dataframe::RankMethod::Average,
                               true)
                    .collect(64));
            const std::vector<double> vmax = column(mx, "v");
            CHECK(vmax[0] == 1.0);
            CHECK(std::isnan(vmax[1]));
            CHECK(std::isnan(vmax[2]));
            CHECK(vmax[3] == 2.0);
            CHECK(vmax[4] == 7.0);
            CHECK(vmax[5] == 3.0);
            CHECK(vmax[6] == 4.0);
            const DataFrame mn = run(
                df.lazy()
                    .memory_budget(budget)
                    .group_by(std::vector<std::string>{"k"})
                    .transform(GroupwiseOp::CumMin, 0,
                               dftracer::utils::dataframe::RankMethod::Average,
                               true)
                    .collect(64));
            const std::vector<double> wmin = column(mn, "w");
            CHECK(wmin[0] == 5.0);
            CHECK(std::isnan(wmin[1]));
            CHECK(wmin[2] == 1.0);
            CHECK(wmin[3] == 2.0);
            CHECK(wmin[4] == 2.0);
        }
    }

    TEST_CASE(
        "group transform under a tiny budget matches the in-memory plan") {
        // Chunks of about 1 KiB cut every partition (about 130 rows) several
        // times, so the carry of each function across a cut is exercised:
        // seed rows (running sum/min/max, fill), context rows (shift, diff,
        // rolling), shifted counts and ranks, and the partition-aligned
        // fallback (average and max rank, float rolling sums).
        const std::int64_t n = 6000;
        auto frame = [&](std::int64_t groups) {
            std::vector<std::int64_t> k(n), v(n);
            std::vector<double> f(n);
            std::vector<std::int32_t> u(n);
            std::vector<std::uint8_t> vv((n + 7) / 8, 0), fv((n + 7) / 8, 0);
            for (std::int64_t i = 0; i < n; ++i) {
                const auto z = static_cast<std::size_t>(i);
                k[z] = (i * 31) % groups;
                v[z] = (i * 17) % 101 - 40;
                f[z] = static_cast<double>(i) * 0.37 - 100.0;
                u[z] = static_cast<std::int32_t>((i * 13) % 59) - 20;
                if (i % 7 != 0)
                    vv[z >> 3] |= static_cast<std::uint8_t>(1u << (z & 7));
                if (i % 11 != 0)
                    fv[z >> 3] |= static_cast<std::uint8_t>(1u << (z & 7));
            }
            DataFrame df;
            df.names = {"k", "v", "f", "u"};
            df.columns.push_back(Series::flat_i64(k.data(), n));
            df.columns.push_back(Series::flat_i64(v.data(), n, vv.data()));
            df.columns.push_back(Series::flat_f64(f.data(), n, fv.data()));
            df.columns.push_back(Series::flat(TypeId::Int32, u.data(), n));
            return df;
        };
        using dftracer::utils::dataframe::RankMethod;
        struct Case {
            GroupwiseOp op;
            std::int64_t n;
            RankMethod method;
        };
        const std::vector<Case> cases = {
            {GroupwiseOp::CumSum, 0, RankMethod::Average},
            {GroupwiseOp::CumMax, 0, RankMethod::Average},
            {GroupwiseOp::CumMin, 0, RankMethod::Average},
            {GroupwiseOp::CumProd, 0, RankMethod::Average},
            {GroupwiseOp::CumCount, 0, RankMethod::Average},
            {GroupwiseOp::Shift, 1, RankMethod::Average},
            {GroupwiseOp::Shift, -2, RankMethod::Average},
            {GroupwiseOp::Shift, 3, RankMethod::Average},
            {GroupwiseOp::Diff, 0, RankMethod::Average},
            {GroupwiseOp::PctChange, 0, RankMethod::Average},
            {GroupwiseOp::FFill, 0, RankMethod::Average},
            {GroupwiseOp::BFill, 0, RankMethod::Average},
            {GroupwiseOp::RollingSum, 3, RankMethod::Average},
            {GroupwiseOp::RollingMin, 4, RankMethod::Average},
            {GroupwiseOp::RollingMax, 2, RankMethod::Average},
            {GroupwiseOp::Rank, 0, RankMethod::Min},
            {GroupwiseOp::Rank, 0, RankMethod::Dense},
            {GroupwiseOp::Rank, 0, RankMethod::Ordinal},
            {GroupwiseOp::Rank, 0, RankMethod::Average},
            {GroupwiseOp::Rank, 0, RankMethod::Max},
            {GroupwiseOp::Head, 2, RankMethod::Average},
            {GroupwiseOp::Tail, 2, RankMethod::Average},
            {GroupwiseOp::Nth, 1, RankMethod::Average},
        };
        const auto same = expect_frames_equal;
        const std::vector<std::vector<std::string>> key_sets = {{"k"}, {}};
        for (const std::int64_t groups : {std::int64_t{47}, std::int64_t{1}}) {
            const DataFrame df = frame(groups);
            for (const std::vector<std::string>& keys : key_sets) {
                if (keys.empty() && groups != 47) continue;
                for (const Case& c : cases) {
                    INFO("op=" << static_cast<int>(c.op) << " n=" << c.n
                               << " method=" << static_cast<int>(c.method)
                               << " groups=" << groups
                               << " keys=" << keys.size());
                    DataFrame want =
                        run(df.lazy()
                                .group_by(keys)
                                .transform(c.op, c.n, c.method, true)
                                .collect(64));
                    DataFrame got =
                        run(df.lazy()
                                .memory_budget(4096)
                                .group_by(keys)
                                .transform(c.op, c.n, c.method, true)
                                .collect(64));
                    same(got, want);
                }
            }
        }
    }

    TEST_CASE("lazy take matches the eager path") {
        DataFrame df = make_df();
        std::vector<std::int64_t> idx{4, 0, 2, 2, 5};
        DataFrame lz = run(df.lazy().take(idx).collect(2));
        DataFrame eg = df.take(idx);
        REQUIRE(lz.num_rows() == eg.num_rows());
        const std::int64_t* la = lz.column("a").data<std::int64_t>();
        const std::int64_t* ea = eg.column("a").data<std::int64_t>();
        for (std::int64_t i = 0; i < lz.num_rows(); ++i) CHECK(la[i] == ea[i]);
    }

    TEST_CASE("reverse and take stay exact when the budget forces spilling") {
        std::vector<std::int64_t> a, s_idx;
        std::vector<std::string> names;
        for (std::int64_t i = 0; i < 5000; ++i) {
            a.push_back(i * 3);
            names.push_back("n" + std::to_string(i % 17));
        }
        DataFrame df;
        df.names = {"a", "s"};
        df.columns.push_back(Series::flat_i64(a.data(), 5000));
        df.columns.push_back(Series::strings(names));

        DataFrame rev = run(df.lazy().memory_budget(1).reverse().collect(64));
        DataFrame erev = df.reverse();
        REQUIRE(rev.num_rows() == 5000);
        for (std::int64_t i = 0; i < 5000; ++i)
            REQUIRE(rev.column("a").data<std::int64_t>()[i] ==
                    erev.column("a").data<std::int64_t>()[i]);

        std::vector<std::int64_t> idx{4999, 0, 2500, 2500, 17, 4998, 3};
        DataFrame tk = run(df.lazy().memory_budget(1).take(idx).collect(64));
        DataFrame etk = df.take(idx);
        REQUIRE(tk.num_rows() == etk.num_rows());
        for (std::int64_t i = 0; i < tk.num_rows(); ++i)
            CHECK(tk.column("a").data<std::int64_t>()[i] ==
                  etk.column("a").data<std::int64_t>()[i]);
    }

    TEST_CASE(
        "a spilled sort orders int64 keys that doubles cannot tell apart") {
        constexpr std::int64_t N = 4000;
        constexpr std::int64_t BASE = std::int64_t{1} << 60;
        std::vector<std::int64_t> k(N), v(N);
        std::uint64_t x = 88172645463325252ULL;
        for (std::int64_t i = 0; i < N; ++i) {
            k[i] = BASE + i;
            v[i] = i;
        }
        for (std::int64_t i = N - 1; i > 0; --i) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            const std::int64_t j = static_cast<std::int64_t>(x % (i + 1));
            std::swap(k[i], k[j]);
        }
        DataFrame df;
        df.names = {"k", "v"};
        df.columns.push_back(Series::flat_i64(k.data(), N));
        df.columns.push_back(Series::flat_i64(v.data(), N));
        for (bool descending : {false, true}) {
            CAPTURE(descending);
            DataFrame out = run(df.lazy()
                                    .memory_budget(1)
                                    .sort_by("k", descending)
                                    .collect(32));
            REQUIRE(out.num_rows() == N);
            const std::int64_t* got = out.column("k").data<std::int64_t>();
            for (std::int64_t i = 0; i < N; ++i)
                REQUIRE(got[i] == (descending ? BASE + N - 1 - i : BASE + i));
        }
    }

    TEST_CASE("take reports an index past the end") {
        DataFrame df = make_df();
        CHECK_THROWS(run(df.lazy().memory_budget(1).take({1, 99}).collect(2)));
    }

    TEST_CASE("lazy filter_mask matches the eager DataFrame::filter(mask)") {
        DataFrame df = make_df();
        Series mask = eval(col(0) > std::int64_t{3}, ptrs(df));
        DataFrame lz = run(df.lazy().filter_mask(mask.share()).collect(2));
        DataFrame eg = df.filter(mask);
        REQUIRE(lz.num_rows() == eg.num_rows());
        const std::int64_t* la = lz.column("a").data<std::int64_t>();
        const std::int64_t* ea = eg.column("a").data<std::int64_t>();
        for (std::int64_t i = 0; i < lz.num_rows(); ++i) CHECK(la[i] == ea[i]);
    }

    TEST_CASE("lazy reverse matches the eager path") {
        DataFrame df = make_df();
        DataFrame lz = run(df.lazy().reverse().collect(2));
        DataFrame eg = df.reverse();
        REQUIRE(lz.num_rows() == eg.num_rows());
        const std::int64_t* la = lz.column("a").data<std::int64_t>();
        const std::int64_t* ea = eg.column("a").data<std::int64_t>();
        for (std::int64_t i = 0; i < lz.num_rows(); ++i) CHECK(la[i] == ea[i]);
    }

    TEST_CASE(
        "lazy sort_by_multi matches the eager path (broadcast and "
        "per-column direction)") {
        std::vector<std::int64_t> k{1, 1, 2, 2, 3};
        std::vector<std::int64_t> v{20, 10, 40, 30, 5};
        DataFrame df;
        df.names = {"k", "v"};
        df.columns.push_back(Series::flat_i64(k.data(), 5));
        df.columns.push_back(Series::flat_i64(v.data(), 5));

        DataFrame lz1 =
            run(df.lazy().sort_by_multi({"k", "v"}, false).collect(2));
        DataFrame eg1 = df.sort_by_multi({"k", "v"}, false);
        REQUIRE(lz1.num_rows() == eg1.num_rows());
        {
            const std::int64_t* lk = lz1.column("k").data<std::int64_t>();
            const std::int64_t* ek = eg1.column("k").data<std::int64_t>();
            const std::int64_t* lv = lz1.column("v").data<std::int64_t>();
            const std::int64_t* ev = eg1.column("v").data<std::int64_t>();
            for (std::int64_t i = 0; i < lz1.num_rows(); ++i) {
                CHECK(lk[i] == ek[i]);
                CHECK(lv[i] == ev[i]);
            }
        }

        DataFrame lz2 =
            run(df.lazy()
                    .sort_by_multi({"k", "v"}, std::vector<bool>{false, true})
                    .collect(2));
        DataFrame eg2 =
            df.sort_by_multi({"k", "v"}, std::vector<bool>{false, true});
        REQUIRE(lz2.num_rows() == eg2.num_rows());
        const std::int64_t* lk = lz2.column("k").data<std::int64_t>();
        const std::int64_t* ek = eg2.column("k").data<std::int64_t>();
        const std::int64_t* lv = lz2.column("v").data<std::int64_t>();
        const std::int64_t* ev = eg2.column("v").data<std::int64_t>();
        for (std::int64_t i = 0; i < lz2.num_rows(); ++i) {
            CHECK(lk[i] == ek[i]);
            CHECK(lv[i] == ev[i]);
        }
    }

    TEST_CASE("predicate pushdown moves a filter before reverse") {
        // reverse only flips row order, so a value-based filter commutes with
        // it and hoists ahead, same reasoning as sort_by.
        auto lf = make_df().lazy().reverse().filter(col(0) > std::int64_t{3});
        const std::string plan = lf.explain();
        const auto fpos = plan.find("filter");
        const auto rpos = plan.find("reverse");
        CHECK(fpos != std::string::npos);
        CHECK(rpos != std::string::npos);
        CHECK(fpos < rpos);  // filter reordered before reverse

        DataFrame df = make_df();
        DataFrame eg =
            df.filter(eval(col(0) > std::int64_t{3}, ptrs(df))).reverse();
        DataFrame lz = run(lf.collect(2));
        REQUIRE(lz.num_rows() == eg.num_rows());
        const std::int64_t* la = lz.column("a").data<std::int64_t>();
        const std::int64_t* ea = eg.column("a").data<std::int64_t>();
        for (std::int64_t i = 0; i < lz.num_rows(); ++i) CHECK(la[i] == ea[i]);
    }

    TEST_CASE("predicate pushdown moves a filter before sort_by_multi") {
        std::vector<std::int64_t> k{3, 1, 2, 1, 3};
        std::vector<std::int64_t> v{1, 2, 3, 4, 5};
        DataFrame df;
        df.names = {"k", "v"};
        df.columns.push_back(Series::flat_i64(k.data(), 5));
        df.columns.push_back(Series::flat_i64(v.data(), 5));

        auto lf = df.lazy()
                      .sort_by_multi({"k", "v"}, false)
                      .filter(col(0) > std::int64_t{1});
        const std::string plan = lf.explain();
        const auto fpos = plan.find("filter");
        const auto spos = plan.find("sort_by_multi");
        CHECK(fpos != std::string::npos);
        CHECK(spos != std::string::npos);
        CHECK(fpos < spos);  // filter reordered before sort_by_multi

        DataFrame eg = df.filter(eval(col(0) > std::int64_t{1}, ptrs(df)))
                           .sort_by_multi({"k", "v"}, false);
        DataFrame lz = run(lf.collect(2));
        REQUIRE(lz.num_rows() == eg.num_rows());
        const std::int64_t* lk = lz.column("k").data<std::int64_t>();
        const std::int64_t* ek = eg.column("k").data<std::int64_t>();
        for (std::int64_t i = 0; i < lz.num_rows(); ++i) CHECK(lk[i] == ek[i]);
    }

    TEST_CASE(
        "predicate pushdown leaves a filter after take (position-sensitive)") {
        // take's indices index into whatever reaches it; hoisting a filter
        // above it would renumber the rows out from under those indices.
        DataFrame df = make_df();
        std::vector<std::int64_t> idx{5, 4, 3, 2, 1, 0};  // full reversal
        auto lf = df.lazy().take(idx).filter(col(0) > std::int64_t{3});
        const std::string plan = lf.explain();
        const auto fpos = plan.find("filter");
        const auto tpos = plan.find("take");
        CHECK(fpos != std::string::npos);
        CHECK(tpos != std::string::npos);
        CHECK(tpos < fpos);  // NOT hoisted: take must run first

        DataFrame taken = df.take(idx);
        DataFrame eg =
            taken.filter(eval(col(0) > std::int64_t{3}, ptrs(taken)));
        DataFrame lz = run(lf.collect(2));
        REQUIRE(lz.num_rows() == eg.num_rows());
        const std::int64_t* la = lz.column("a").data<std::int64_t>();
        const std::int64_t* ea = eg.column("a").data<std::int64_t>();
        for (std::int64_t i = 0; i < lz.num_rows(); ++i) CHECK(la[i] == ea[i]);
    }

    TEST_CASE("filter -> take -> select: projection pushdown, no hoist") {
        DataFrame df = make_df();
        std::vector<std::int64_t> idx{0, 2, 1};
        auto lf =
            df.lazy().filter(col(0) > std::int64_t{1}).take(idx).select({"a"});
        const std::string plan = lf.explain();
        const auto proj = plan.find("select [a]");
        const auto filt = plan.rfind("filter");
        CHECK(proj != std::string::npos);
        CHECK(filt != std::string::npos);
        CHECK(proj < filt);  // projection pushed ahead of filter+take

        DataFrame filtered =
            df.filter(eval(col(0) > std::int64_t{1}, ptrs(df)));
        DataFrame eg = filtered.take(idx).select({"a"});
        DataFrame lz = run(lf.collect(2));
        REQUIRE(lz.num_rows() == eg.num_rows());
        const std::int64_t* la = lz.column("a").data<std::int64_t>();
        const std::int64_t* ea = eg.column("a").data<std::int64_t>();
        for (std::int64_t i = 0; i < lz.num_rows(); ++i) CHECK(la[i] == ea[i]);
    }

    TEST_CASE("filter -> reverse -> select: projection pushdown") {
        DataFrame df = make_df();
        auto lf =
            df.lazy().filter(col(0) > std::int64_t{2}).reverse().select({"a"});
        const std::string plan = lf.explain();
        CHECK(plan.find("select [a]") != std::string::npos);
        CHECK(plan.find("reverse") != std::string::npos);

        DataFrame filtered =
            df.filter(eval(col(0) > std::int64_t{2}, ptrs(df)));
        DataFrame eg = filtered.reverse().select({"a"});
        DataFrame lz = run(lf.collect(2));
        REQUIRE(lz.num_rows() == eg.num_rows());
        const std::int64_t* la = lz.column("a").data<std::int64_t>();
        const std::int64_t* ea = eg.column("a").data<std::int64_t>();
        for (std::int64_t i = 0; i < lz.num_rows(); ++i) CHECK(la[i] == ea[i]);
    }

    TEST_CASE("filter -> filter_mask -> select: projection pushdown") {
        // filter_mask's mask is positionally aligned to its OWN input stream,
        // so it is built against the already-filtered frame (5 rows), not the
        // original.
        DataFrame df = make_df();
        DataFrame filtered =
            df.filter(eval(col(0) > std::int64_t{1}, ptrs(df)));
        Series mask =
            eval(col(1) < std::int64_t{60}, ptrs(filtered));  // b < 60

        auto lf = df.lazy()
                      .filter(col(0) > std::int64_t{1})
                      .filter_mask(mask.share())
                      .select({"a"});
        const std::string plan = lf.explain();
        CHECK(plan.find("select [a]") != std::string::npos);
        CHECK(plan.find("filter_mask") != std::string::npos);

        DataFrame eg = filtered.filter(mask).select({"a"});
        DataFrame lz = run(lf.collect(2));
        REQUIRE(lz.num_rows() == eg.num_rows());
        const std::int64_t* la = lz.column("a").data<std::int64_t>();
        const std::int64_t* ea = eg.column("a").data<std::int64_t>();
        for (std::int64_t i = 0; i < lz.num_rows(); ++i) CHECK(la[i] == ea[i]);
    }

    TEST_CASE("sample / is_duplicated / group_by_dynamic") {
        DataFrame s = run(make_df().lazy().sample(3, 42).collect(2));
        CHECK(s.num_rows() == 3);

        // Streaming min-hash sample must match eager DataFrame::sample exactly,
        // across a multi-morsel scan (bounded state, same survivors + order).
        std::vector<std::int64_t> big(100);
        for (std::int64_t i = 0; i < 100; ++i)
            big[static_cast<std::size_t>(i)] = i;
        DataFrame bf;
        bf.names = {"x"};
        bf.columns.push_back(Series::flat_i64(big.data(), 100));
        DataFrame lz = run(bf.lazy().sample(7, 123).collect(8));  // morsel 8
        DataFrame eg = bf.sample(7, 123);
        REQUIRE(lz.num_rows() == eg.num_rows());
        REQUIRE(lz.num_rows() == 7);
        const std::int64_t* lp = lz.column("x").data<std::int64_t>();
        const std::int64_t* ep = eg.column("x").data<std::int64_t>();
        for (std::int64_t i = 0; i < 7; ++i) CHECK(lp[i] == ep[i]);

        // is_duplicated / is_unique: two-pass, per-row mask in input order.
        // Must match eager across a multi-morsel scan.
        std::vector<std::int64_t> d{1, 1, 2, 3, 2, 1};
        DataFrame df;
        df.names = {"x"};
        df.columns.push_back(Series::flat_i64(d.data(), 6));
        auto bit = [](const Series& col, std::int64_t i) {
            return (col.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1;
        };
        DataFrame du = run(df.lazy().is_duplicated().collect(2));
        CHECK(du.names == std::vector<std::string>{"is_duplicated"});
        REQUIRE(du.num_rows() == 6);
        Series ed = df.is_duplicated();
        for (std::int64_t i = 0; i < 6; ++i)
            CHECK(bit(du.column("is_duplicated"), i) == bit(ed, i));

        DataFrame uq = run(df.lazy().is_unique().collect(2));
        CHECK(uq.names == std::vector<std::string>{"is_unique"});
        Series eu = df.is_unique();
        for (std::int64_t i = 0; i < 6; ++i)
            CHECK(bit(uq.column("is_unique"), i) == bit(eu, i));

        std::vector<std::int64_t> t{0, 1, 2, 3}, v{1, 1, 1, 1};
        DataFrame tf;
        tf.names = {"t", "v"};
        tf.columns.push_back(Series::flat_i64(t.data(), 4));
        tf.columns.push_back(Series::flat_i64(v.data(), 4));
        std::vector<GroupAgg> aggs{{Agg::Sum, "v", "sum", 0.0}};
        DataFrame gd =
            run(tf.lazy().group_by_dynamic("t", 2, 2, aggs).collect(2));
        CHECK(gd.names == std::vector<std::string>{"t", "sum"});
        CHECK(gd.num_rows() == 2);  // windows [0,2), [2,4)
        CHECK(gd.column("sum").data<std::int64_t>()[0] == 2);

        // Streaming windowed agg must match eager across a multi-morsel scan,
        // including a sliding window (period > every). Ascending time.
        std::vector<std::int64_t> bt(60), bv(60);
        for (std::int64_t i = 0; i < 60; ++i) {
            bt[static_cast<std::size_t>(i)] = i;
            bv[static_cast<std::size_t>(i)] = i * 2;
        }
        DataFrame wf;
        wf.names = {"t", "v"};
        wf.columns.push_back(Series::flat_i64(bt.data(), 60));
        wf.columns.push_back(Series::flat_i64(bv.data(), 60));
        std::vector<GroupAgg> wa{{Agg::Sum, "v", "s", 0.0},
                                 {Agg::Count, "", "c", 0.0}};
        DataFrame wl = run(
            wf.lazy().group_by_dynamic("t", 5, 12, wa).collect(7));  // sliding
        DataFrame we = wf.group_by_dynamic("t", 5, 12, wa);
        REQUIRE(wl.num_rows() == we.num_rows());
        const std::int64_t* lt = wl.column("t").data<std::int64_t>();
        const std::int64_t* et = we.column("t").data<std::int64_t>();
        const std::int64_t* ls = wl.column("s").data<std::int64_t>();
        const std::int64_t* es = we.column("s").data<std::int64_t>();
        const std::int64_t* lc = wl.column("c").data<std::int64_t>();
        const std::int64_t* ec = we.column("c").data<std::int64_t>();
        for (std::int64_t i = 0; i < wl.num_rows(); ++i) {
            CHECK(lt[i] == et[i]);
            CHECK(ls[i] == es[i]);
            CHECK(lc[i] == ec[i]);
        }
    }

    TEST_CASE("data-dependent schema: pivot / to_dummies / describe") {
        // to_dummies: one Int8 column per distinct value of "g".
        std::vector<std::int64_t> g{0, 1, 0};
        DataFrame gf;
        gf.names = {"g"};
        gf.columns.push_back(Series::flat_i64(g.data(), 3));
        LazyFrame dl = gf.lazy().to_dummies("g");
        CHECK(dl.schema().empty());    // unknown until run
        DataFrame d = run(dl.collect(2));
        DataFrame dd = gf.to_dummies("g");
        REQUIRE(d.names == dd.names);  // g_0, g_1 in ascending order
        REQUIRE(d.num_rows() == dd.num_rows());
        for (const std::string& cn : d.names) {
            const std::int8_t* a = d.column(cn).data<std::int8_t>();
            const std::int8_t* b = dd.column(cn).data<std::int8_t>();
            for (std::int64_t r = 0; r < d.num_rows(); ++r) CHECK(a[r] == b[r]);
        }

        // pivot: rows = distinct index, one value column per distinct "on".
        std::vector<std::int64_t> i{0, 0, 1, 1}, k{10, 20, 10, 20},
            v{1, 2, 3, 4};
        DataFrame pf;
        pf.names = {"i", "k", "v"};
        pf.columns.push_back(Series::flat_i64(i.data(), 4));
        pf.columns.push_back(Series::flat_i64(k.data(), 4));
        pf.columns.push_back(Series::flat_i64(v.data(), 4));
        DataFrame p = run(pf.lazy().pivot("i", "k", "v", "sum").collect(2));
        DataFrame pe = pf.pivot("i", "k", "v", "sum");
        REQUIRE(p.names == pe.names);  // i, 10, 20 (ascending on-values)
        REQUIRE(p.num_rows() == pe.num_rows());
        for (const std::string& cn : p.names) {
            const std::int64_t* a = p.column(cn).data<std::int64_t>();
            const std::int64_t* b = pe.column(cn).data<std::int64_t>();
            for (std::int64_t r = 0; r < p.num_rows(); ++r) CHECK(a[r] == b[r]);
        }

        // describe: streaming stats must match eager, across small morsels.
        DataFrame ds = run(make_df().lazy().describe().collect(2));
        DataFrame de = make_df().describe();
        REQUIRE(ds.names == de.names);
        REQUIRE(ds.num_rows() == de.num_rows());  // 6 statistics
        for (const std::string& cn : {std::string("a"), std::string("b")}) {
            const double* s = ds.column(cn).data<double>();
            const double* e = de.column(cn).data<double>();
            for (std::int64_t r = 0; r < ds.num_rows(); ++r)
                CHECK(s[r] == doctest::Approx(e[r]));
        }
    }

    TEST_CASE("two-pass sinks spill the input under a tiny budget") {
        // memory_budget(1) forces the input spool to disk between passes; the
        // results must still match the in-memory path.
        std::vector<std::int64_t> x{5, 3, 5, 1, 3, 5, 2, 1};
        DataFrame df;
        df.names = {"x"};
        df.columns.push_back(Series::flat_i64(x.data(), 8));
        auto bit = [](const Series& s, std::int64_t i) {
            return (s.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1;
        };
        DataFrame du =
            run(df.lazy().memory_budget(1).is_duplicated().collect(2));
        Series ed = df.is_duplicated();
        REQUIRE(du.num_rows() == 8);
        for (std::int64_t i = 0; i < 8; ++i)
            CHECK(bit(du.column("is_duplicated"), i) == bit(ed, i));

        std::vector<std::int64_t> i2{0, 0, 1, 1}, k2{10, 20, 10, 20},
            v2{1, 2, 3, 4};
        DataFrame pf;
        pf.names = {"i", "k", "v"};
        pf.columns.push_back(Series::flat_i64(i2.data(), 4));
        pf.columns.push_back(Series::flat_i64(k2.data(), 4));
        pf.columns.push_back(Series::flat_i64(v2.data(), 4));
        DataFrame p = run(
            pf.lazy().memory_budget(1).pivot("i", "k", "v", "sum").collect(2));
        DataFrame pe = pf.pivot("i", "k", "v", "sum");
        REQUIRE(p.names == pe.names);
        for (const std::string& cn : p.names) {
            const std::int64_t* a = p.column(cn).data<std::int64_t>();
            const std::int64_t* b = pe.column(cn).data<std::int64_t>();
            for (std::int64_t r = 0; r < p.num_rows(); ++r) CHECK(a[r] == b[r]);
        }
    }

    TEST_CASE(
        "pivot under a tiny budget matches eager over many index values") {
        const std::int64_t n = 3000;
        std::vector<std::int64_t> k(n), v(n);
        std::vector<std::string> i(n);
        for (std::int64_t r = 0; r < n; ++r) {
            const std::int64_t h = (r * 2654435761LL) % 1009;
            i[static_cast<std::size_t>(r)] = "row" + std::to_string(h);
            k[static_cast<std::size_t>(r)] = (r * 7 + h) % 5;
            v[static_cast<std::size_t>(r)] = r;
        }
        DataFrame pf;
        pf.names = {"i", "k", "v"};
        pf.columns.push_back(Series::strings(i));
        pf.columns.push_back(Series::flat_i64(k.data(), n));
        pf.columns.push_back(Series::flat_i64(v.data(), n));
        for (const char* agg : {"first", "last", "sum", "max"}) {
            DataFrame p = run(pf.lazy()
                                  .memory_budget(1)
                                  .pivot("i", "k", "v", agg)
                                  .collect(7));
            DataFrame pe = pf.pivot("i", "k", "v", agg);
            REQUIRE(p.names == pe.names);
            REQUIRE(p.num_rows() == pe.num_rows());
            for (std::int64_t r = 0; r < p.num_rows(); ++r)
                CHECK(p.column("i").string_at(r) ==
                      pe.column("i").string_at(r));
            for (std::size_t c = 1; c < p.names.size(); ++c) {
                const Series& a = p.columns[c];
                const Series& b = pe.columns[c];
                for (std::int64_t r = 0; r < p.num_rows(); ++r) {
                    REQUIRE(a.is_null(r) == b.is_null(r));
                    if (!a.is_null(r))
                        CHECK(a.data<std::int64_t>()[r] ==
                              b.data<std::int64_t>()[r]);
                }
            }
        }
    }

    TEST_CASE("a comparison the column type cannot take throws") {
        std::vector<std::string> names{"a", "b", "c"};
        DataFrame f;
        f.names = {"name"};
        f.columns.push_back(Series::strings(names));
        LazyFrame lf = f.lazy().with_column("big", col(0) > std::int64_t{25});
        CHECK_THROWS_AS(run(lf.collect()), std::invalid_argument);
    }

    TEST_CASE("predicate pushdown keeps a dependent filter after with_column") {
        // Filter on 'c' (col 2, the added column) must NOT move up.
        auto lf = make_df()
                      .lazy()
                      .with_column("c", col(0) + col(1))
                      .filter(col(2) > std::int64_t{50});  // c > 50
        const std::string plan = lf.explain();
        CHECK(plan.find("with_column") < plan.find("filter"));

        DataFrame r = run(lf.collect());
        // c = a+b in {11,22,33,44,55,66}; c > 50 -> rows 5,6 (c=55,66).
        CHECK(r.num_rows() == 2);
        CHECK(r.column("c").data<std::int64_t>()[0] == 55);
    }

    TEST_CASE(
        "InMemorySource projection pushdown returns only wanted columns") {
        using dftracer::utils::dataframe::InMemorySource;
        using dftracer::utils::dataframe::Pushed;
        using dftracer::utils::dataframe::ScanRequest;
        using dftracer::utils::dataframe::ScanResult;

        InMemorySource src(make_df());  // columns a, b
        CHECK(src.names() == std::vector<std::string>{"a", "b"});

        ScanRequest req;
        req.projection = {"b"};
        req.filters.push_back(col(0) > std::int64_t{1});
        ScanResult r = src.scan(req);

        // The whole-column engine applies the predicate, so the source leaves
        // it.
        REQUIRE(r.filters.size() == 1);
        CHECK(r.filters[0] == Pushed::No);

        DataFrame got = dftracer::utils::default_runtime()
                            .submit(drain_cursor(std::move(r.cursor)))
                            .get();
        REQUIRE(got.num_columns() == 1);  // only "b" was harvested
        REQUIRE(got.num_rows() == 6);
        const std::int64_t* bv = got.columns[0].data<std::int64_t>();
        CHECK(bv[0] == 10);
        CHECK(bv[5] == 60);
    }

    TEST_CASE("InMemorySource schema() round-trips a nested Struct column") {
        using dftracer::utils::dataframe::Field;
        using dftracer::utils::dataframe::InMemorySource;
        using dftracer::utils::dataframe::Schema;
        using dftracer::utils::dataframe::TypeId;

        std::vector<std::int64_t> lo{1, 2};
        std::vector<double> hi{1.5, 2.5};
        std::vector<Series> fields;
        fields.push_back(Series::flat_i64(lo.data(), 2));
        fields.push_back(Series::flat_f64(hi.data(), 2));
        Series st = Series::structs({"lo", "hi"}, std::move(fields));

        DataFrame df;
        df.names = {"a", "range"};
        df.columns.push_back(Series::flat_i64(lo.data(), 2));
        df.columns.push_back(std::move(st));

        InMemorySource src(std::move(df));
        Schema schema = src.schema();
        REQUIRE(schema.fields.size() == 2);
        CHECK(schema.fields[0].name == "a");
        CHECK(schema.fields[0].type.id == TypeId::Int64);
        CHECK(schema.fields[1].name == "range");
        REQUIRE(schema.fields[1].type.id == TypeId::Struct);
        REQUIRE(schema.fields[1].type.fields.size() == 2);
        CHECK(schema.fields[1].type.fields[0].name == "lo");
        CHECK(schema.fields[1].type.fields[0].type.id == TypeId::Int64);
        CHECK(schema.fields[1].type.fields[1].name == "hi");
        CHECK(schema.fields[1].type.fields[1].type.id == TypeId::Float64);
    }

    TEST_CASE("streaming source with per-morsel schema reconciles by name") {
        auto source = std::make_shared<RaggedSource>();
        DataFrame r = run(LazyFrame::scan(source).collect());

        REQUIRE(r.num_rows() == 3);
        REQUIRE(r.column_index("a") >= 0);
        REQUIRE(r.column_index("b") >= 0);
        REQUIRE(r.column_index("c") >= 0);

        const Series& a =
            r.columns[static_cast<std::size_t>(r.column_index("a"))];
        CHECK(a.type() == dftracer::utils::dataframe::TypeId::Float64);
        const double* av = a.data<double>();
        CHECK(av[0] == doctest::Approx(1.0));
        CHECK(av[1] == doctest::Approx(2.0));
        CHECK(av[2] == doctest::Approx(3.5));

        const Series& b =
            r.columns[static_cast<std::size_t>(r.column_index("b"))];
        CHECK_FALSE(b.is_null(0));
        CHECK_FALSE(b.is_null(1));
        CHECK(b.is_null(2));

        const Series& c =
            r.columns[static_cast<std::size_t>(r.column_index("c"))];
        CHECK(c.is_null(0));
        CHECK(c.is_null(1));
        CHECK_FALSE(c.is_null(2));
        CHECK(c.string_at(2) == "x");
    }

    TEST_CASE("stream() yields morsels and collect() equals draining it") {
        auto lf = make_df().lazy().filter(col(0) > std::int64_t{3});

        auto [chunk_rows, streamed] = run_stream_probe(lf.stream(2));

        DataFrame collected = run(lf.collect(2));

        REQUIRE(chunk_rows.size() == 2);
        CHECK(chunk_rows[0] == 1);
        CHECK(chunk_rows[1] == 2);
        REQUIRE(streamed.num_rows() == 3);
        REQUIRE(streamed.num_rows() == collected.num_rows());
        REQUIRE(streamed.column_index("a") >= 0);
        const Series& a =
            streamed
                .columns[static_cast<std::size_t>(streamed.column_index("a"))];
        CHECK(a.data<std::int64_t>()[0] == 4);
        CHECK(a.data<std::int64_t>()[2] == 6);
    }

    TEST_CASE("DataFrame::stream() yields fixed-size row slices") {
        DataFrame df = make_df().slice(0, 5);  // 6 rows -> a 5-row prefix

        auto [chunk_rows, streamed] = run_stream_probe(df.stream(2));

        REQUIRE(chunk_rows.size() == 3);
        CHECK(chunk_rows[0] == 2);
        CHECK(chunk_rows[1] == 2);
        CHECK(chunk_rows[2] == 1);
        REQUIRE(streamed.num_rows() == 5);
        const Series& a =
            streamed
                .columns[static_cast<std::size_t>(streamed.column_index("a"))];
        const Series& orig_a =
            df.columns[static_cast<std::size_t>(df.column_index("a"))];
        for (std::int64_t i = 0; i < 5; ++i)
            CHECK(a.data<std::int64_t>()[i] == orig_a.data<std::int64_t>()[i]);
    }

    TEST_CASE("DataFrame::stream() yields nothing for an empty frame") {
        DataFrame df = make_df().slice(0, 0);
        auto [chunk_rows, streamed] = run_stream_probe(df.stream(2));
        CHECK(chunk_rows.empty());
        CHECK(streamed.num_columns() == 0);
    }
}
