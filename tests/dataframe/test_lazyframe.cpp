#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/agg_expr.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <doctest/doctest.h>

#include <cstdint>
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
using dftracer::utils::dataframe::LazyFrame;
using dftracer::utils::dataframe::Morsel;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::Source;

namespace {

DataFrame run(CoroTask<DataFrame> t) {
    return dftracer::utils::default_runtime().submit(std::move(t)).get();
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
            m.name_ids = {intern_->get_or_insert("a"),
                          intern_->get_or_insert("b")};
            m.intern = intern_;
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
            m.name_ids = {intern_->get_or_insert("a"),
                          intern_->get_or_insert("c")};
            m.intern = intern_;
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
