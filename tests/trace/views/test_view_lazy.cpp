#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/cell_ops.h>
#include <dftracer/utils/dataframe/internal/lazy_plan.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/trace/views/view_source.h>
#include <doctest/doctest.h>

#include <sstream>
#include <string>
#include <vector>

#include "test_view_common.h"

namespace scan = dftracer::utils::trace::views::detail::scan;

TEST_SUITE("trace scan - lazy collect") {
    TEST_CASE(
        "scan::collect() returns a LazyFrame matching scan::collect_frame()") {
        const auto& s = shared_trace();
        scan::ScanPlan v = scan::agg(
            scan::group_by(scan::metadata(scan::from_file(s.gz, s.idx), false),
                           {GroupKey::cat()}),
            {{AggOp::Count, "", "n"}});

        dataframe::LazyFrame lz = scan::collect(v);
        dataframe::DataFrame via_lazy = run(lz.collect());
        dataframe::DataFrame via_eager = run(scan::collect_frame(v));

        REQUIRE(via_lazy.num_rows() == via_eager.num_rows());
        REQUIRE(via_lazy.num_rows() == 2);
        for (std::int64_t i = 0; i < via_lazy.num_rows(); ++i) {
            CHECK(bstr(via_lazy, i, "cat") == bstr(via_eager, i, "cat"));
            CHECK(bnum(via_lazy, i, "n") == bnum(via_eager, i, "n"));
        }
    }

    TEST_CASE("scan::collect() lazy plan composes filter + select") {
        const auto& s = shared_trace();
        scan::ScanPlan base =
            scan::metadata(scan::from_file(s.gz, s.idx), false);

        dataframe::DataFrame all_events = run(scan::collect_frame(base));

        dataframe::LazyFrame lz =
            scan::collect(scan::query(base, R"(cat == "POSIX")"))
                .select({"cat", "name"});
        dataframe::DataFrame subset = run(lz.collect());

        CHECK(subset.num_rows() == 30);
        REQUIRE(bhas(subset, "cat"));
        REQUIRE(bhas(subset, "name"));
        CHECK(subset.columns.size() == 2);
        for (std::int64_t i = 0; i < subset.num_rows(); ++i)
            CHECK(bstr(subset, i, "cat") == "POSIX");
        CHECK(all_events.num_rows() == 50);
    }
}

// View: plan equality against the scan builders, source absorption and
// builder order.

using dftracer::utils::dataframe::Agg;
using dftracer::utils::dataframe::CmpOp;
using dftracer::utils::dataframe::col;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::expr_cmp;
using dftracer::utils::dataframe::LazyFrame;
using dftracer::utils::dataframe::str;
using dftracer::utils::dataframe::detail::optimize_plan;
using dftracer::utils::dataframe::detail::plan_source;
using test_view_common::run;

namespace {

// Every ViewPlan field a scan reads, in a fixed order.
std::string canonical(const trace::views::detail::ViewPlan& p) {
    std::ostringstream o;
    for (const ViewFile& f : p.files) o << f.file_path << '|' << f.index_path;
    o << "\nquery=" << (p.query ? p.query->source() : "");
    o << "\nrange=";
    if (p.time_range) o << p.time_range->first << ',' << p.time_range->second;
    o << "\nphase=" << static_cast<int>(p.phase)
      << " meta=" << p.include_metadata << p.emit_all_metadata
      << " bucket=" << p.time_bucket_us << ',' << p.bucket_origin_us << ','
      << p.bucket_origin_min << " occ=" << p.occ_cell_us
      << " scale=" << p.time_scale;
    o << "\ngroup=";
    for (const GroupKey& k : p.group_by) {
        o << static_cast<int>(k.kind) << ':' << k.arg << ':'
          << static_cast<int>(k.transform);
        for (const std::string& a : k.transform_args) o << ':' << a;
        o << ';';
    }
    o << "\nagg=";
    for (const AggSpec& a : p.agg)
        o << static_cast<int>(a.op) << ':' << a.field << ':' << a.out_name
          << ':' << a.by << ':' << a.q << ';';
    o << "\nnumeric=" << p.auto_numeric_metrics << ':';
    for (const AggSpec& a : p.numeric_arg_aggs)
        o << static_cast<int>(a.op) << ';';
    o << "\nbudget=" << p.memory_budget << "\nselect=";
    for (const std::string& s : p.select) o << s << ';';
    o << "\npage=" << p.limit << ',' << p.offset << " sort=" << p.sort_col
      << ',' << p.sort_desc << " topk=" << p.topk_col << ',' << p.topk_k << ','
      << p.topk_largest;
    o << "\nroots=" << p.rollup_root << '|' << p.views_root
      << " mat=" << p.materialize;
    return o.str();
}

// The optimized plan: the scan the source ends with, then the engine ops.
std::string optimized(const LazyFrame& lf) {
    LazyFrame opt = optimize_plan(lf);
    const auto* src = dynamic_cast<const ViewSource*>(plan_source(opt).get());
    REQUIRE(src != nullptr);
    std::string ops = opt.explain();
    ops = ops.substr(ops.find('\n') == std::string::npos ? ops.size()
                                                         : ops.find('\n'));
    return canonical(*src->plan()) + "\nops:" + ops;
}

void check_same_rows(const DataFrame& a, const DataFrame& b) {
    REQUIRE(a.names == b.names);
    REQUIRE(a.num_rows() == b.num_rows());
    for (std::size_t c = 0; c < a.columns.size(); ++c)
        for (std::int64_t r = 0; r < a.num_rows(); ++r)
            CHECK(dftracer::utils::dataframe::cell_to_string(a.columns[c], r) ==
                  dftracer::utils::dataframe::cell_to_string(b.columns[c], r));
}

scan::ScanPlan base_plan() {
    const auto& s = shared_trace();
    return scan::metadata(scan::from_file(s.gz, s.idx), false);
}

View base_viewer() {
    const auto& s = shared_trace();
    return View::from_file(s.gz, s.idx).metadata(false);
}

}  // namespace

TEST_SUITE("View - scan plan equality") {
    TEST_CASE("an aggregation with sort and limit") {
        scan::ScanPlan v = scan::limit(
            scan::sort_by(
                scan::agg(scan::group_by(base_plan(), {GroupKey::cat()}),
                          {{AggOp::Count, "", "n"}}),
                "n", true),
            1);
        View t = base_viewer()
                     .group_by({GroupKey::cat()})
                     .agg({{AggOp::Count, "", "n"}})
                     .sort_by("n", true)
                     .slice(0, 1);
        CHECK(optimized(scan::collect(v)) == optimized(t.lazy()));
        check_same_rows(run(scan::collect(v).collect()), run(t.collect()));
    }

    TEST_CASE("an event filter, phase and time range before a group-by") {
        scan::ScanPlan v = scan::agg(
            scan::group_by(
                scan::time_range(
                    scan::phase(scan::query(base_plan(), R"(cat == "POSIX")"),
                                Phase::Events),
                    1000, 3000),
                {GroupKey::name()}),
            {{AggOp::Sum, "dur", "sum_dur"}});
        View t = base_viewer()
                     .query(R"(cat == "POSIX")")
                     .phase(Phase::Events)
                     .time_range(1000, 3000)
                     .group_by({GroupKey::name()})
                     .agg({{AggOp::Sum, "dur", "sum_dur"}});
        CHECK(optimized(scan::collect(v)) == optimized(t.lazy()));
        check_same_rows(run(scan::collect(v).collect()), run(t.collect()));
    }

    TEST_CASE("a time bucket with top-k") {
        scan::ScanPlan v = scan::topk(
            scan::agg(scan::group_by(scan::time_bucket(base_plan(), 1000),
                                     {GroupKey::cat()}),
                      {{AggOp::Count, "", "n"}}),
            "n", 2);
        View t = base_viewer()
                     .time_bucket(1000)
                     .group_by({GroupKey::cat()})
                     .agg({{AggOp::Count, "", "n"}})
                     .topk("n", 2);
        CHECK(optimized(scan::collect(v)) == optimized(t.lazy()));
        check_same_rows(run(scan::collect(v).collect()), run(t.collect()));
    }

    TEST_CASE("a row query with a select") {
        scan::ScanPlan v = scan::select(
            scan::query(base_plan(), R"(cat == "STDIO")"), {"cat", "name"});
        View t =
            base_viewer().query(R"(cat == "STDIO")").select({"cat", "name"});
        CHECK(optimized(scan::collect(v)) == optimized(t.lazy()));
        check_same_rows(run(scan::collect(v).collect()), run(t.collect()));
    }

    TEST_CASE("a trace aggregation keeps its post-scan sort") {
        scan::ScanPlan v = scan::sort_by(
            scan::agg(scan::group_by(base_plan(), {GroupKey::cat()}),
                      {{AggOp::Count, "", "n"}}),
            "cat");
        View t = base_viewer()
                     .group_by({GroupKey::cat()})
                     .agg({{AggOp::Count, "", "n"}})
                     .sort_by("cat");
        CHECK(optimized(scan::collect(v)) == optimized(t.lazy()));
        check_same_rows(run(scan::collect(v).collect()), run(t.collect()));
    }
}

TEST_SUITE("View - source absorption") {
    TEST_CASE("an Expr filter is absorbed like the DSL filter") {
        View t = base_viewer();
        const std::vector<std::string> names = t.schema();
        std::int32_t cat = -1;
        for (std::size_t i = 0; i < names.size(); ++i)
            if (names[i] == "cat") cat = static_cast<std::int32_t>(i);
        REQUIRE(cat >= 0);
        View by_expr = t.filter(expr_cmp(CmpOp::Eq, col(cat), str("POSIX")));
        CHECK(by_expr.explain().find("filter") == std::string::npos);
        DataFrame a = run(by_expr.collect());
        DataFrame b = run(t.query(R"(cat == "POSIX")").collect());
        CHECK(a.num_rows() == 30);
        CHECK(a.num_rows() == b.num_rows());
    }

    TEST_CASE("a generic group-by on fixed fields is absorbed") {
        View t = base_viewer();
        View generic =
            t.group_by(std::vector<std::string>{"name"},
                       {{Agg::Count, "", "n"}, {Agg::Sum, "dur", "sum_dur"}})
                .sort_by("name");
        CHECK(generic.explain().find("group_by") == std::string::npos);
        View traced =
            t.group_by({GroupKey::name()})
                .agg({{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "sum_dur"}})
                .sort_by("name");
        check_same_rows(run(generic.collect()), run(traced.collect()));
    }

    TEST_CASE("a generic group-by on cat keeps its raw case") {
        View g = base_viewer()
                     .group_by(std::vector<std::string>{"cat"},
                               {{Agg::Count, "", "n"}})
                     .sort_by("cat");
        CHECK(g.explain().find("group_by") == std::string::npos);
        DataFrame r = run(g.collect());
        REQUIRE(r.num_rows() == 2);
        CHECK(bstr(r, 0, "cat") == "POSIX");
        CHECK(bstr(r, 1, "cat") == "STDIO");
    }

    TEST_CASE(
        "a group-by on any other field is absorbed as an expression key") {
        View t = base_viewer();
        View g =
            t.group_by(std::vector<std::string>{"ph"}, {{Agg::Count, "", "n"}});
        CHECK(g.explain().find("group_by") == std::string::npos);
        DataFrame r = run(g.collect());
        REQUIRE(r.num_rows() == 1);
        CHECK(bnum(r, 0, "n") == 50);
    }

    TEST_CASE("an aggregated scan keeps a later Expr filter as an engine op") {
        View t = base_viewer()
                     .group_by({GroupKey::cat()})
                     .agg({{AggOp::Count, "", "n"}});
        View f = t.filter(col(1) > std::int64_t{25});
        CHECK(f.explain().find("filter") != std::string::npos);
        DataFrame r = run(f.collect());
        REQUIRE(r.num_rows() == 1);
        // The view's cat key is lowercased.
        CHECK(bstr(r, 0, "cat") == "posix");
    }
}

TEST_SUITE("View - builder order") {
    TEST_CASE("a trace builder after a non-filter op is refused") {
        View t = base_viewer().sort_by("ts");
        CHECK_THROWS_WITH_AS(t.phase(Phase::Events),
                             doctest::Contains("sort_by"), DFTUtilsException);
    }

    TEST_CASE("a trace builder may follow an Expr filter") {
        View t = base_viewer().filter(col(0) > std::int64_t{0});
        CHECK_NOTHROW(t.phase(Phase::Events));
    }

    TEST_CASE("the chain keeps the View type") {
        View t = base_viewer().select({"cat", "name"}).head(3);
        CHECK(run(t.collect()).num_rows() == 3);
    }
}

namespace {

std::shared_ptr<const ViewSource> absorbed_source(const LazyFrame& lf) {
    return std::dynamic_pointer_cast<const ViewSource>(
        plan_source(optimize_plan(lf)));
}

std::vector<LazyFrame> mixed_plans() {
    View t = base_viewer();
    return {
        t.group_by({GroupKey::cat()})
            .agg({{AggOp::Count, "", "n"}})
            .sort_by("cat")
            .lazy(),
        t.query(R"(cat == "POSIX")")
            .group_by({GroupKey::name()})
            .agg({{AggOp::Sum, "dur", "sum_dur"}})
            .lazy(),
        t.query(R"(cat == "STDIO")").select({"cat", "name", "ts"}).lazy(),
        t.phase(Phase::Events).select({"name", "dur"}).sort_by("dur").lazy(),
    };
}

}  // namespace

TEST_SUITE("View - collect_all and sessions") {
    TEST_CASE("collect_all matches collecting each plan alone") {
        std::vector<LazyFrame> plans = mixed_plans();
        std::vector<DataFrame> together =
            run(dftracer::utils::dataframe::collect_all(plans));
        REQUIRE(together.size() == plans.size());
        for (std::size_t i = 0; i < plans.size(); ++i)
            check_same_rows(together[i], run(plans[i].collect()));
    }

    TEST_CASE("plans on one trace base share the batch key") {
        std::vector<LazyFrame> plans = mixed_plans();
        std::optional<std::string> key = absorbed_source(plans[0])->batch_key();
        REQUIRE(key);
        for (const LazyFrame& lf : plans)
            CHECK(absorbed_source(lf)->batch_key() == key);
    }

    TEST_CASE("a batch scans the trace once for every branch") {
        std::vector<std::shared_ptr<const ViewSource>> members;
        for (const LazyFrame& lf : mixed_plans())
            members.push_back(absorbed_source(lf));
        ViewSource::Batch b = run(ViewSource::run_batch(members));
        REQUIRE(b.frames.size() == members.size());
        CHECK(b.stats.events_scanned == 50);
    }

    TEST_CASE("a different time range is a different base") {
        View t = base_viewer();
        View early = t.time_range(0, 3000);
        CHECK(absorbed_source(t.lazy())->batch_key() !=
              absorbed_source(early.lazy())->batch_key());
        std::vector<DataFrame> r = run(
            dftracer::utils::dataframe::collect_all({t.lazy(), early.lazy()}));
        CHECK(r[0].num_rows() == 50);
        CHECK(r[1].num_rows() == run(early.collect()).num_rows());
    }

    TEST_CASE("collect_all mixes trace and in-memory plans") {
        DataFrame small;
        std::vector<std::int64_t> a{1, 2, 3};
        small.names = {"a"};
        small.columns.push_back(
            dftracer::utils::dataframe::Series::flat_i64(a.data(), 3));
        View t = base_viewer();
        std::vector<DataFrame> r = run(dftracer::utils::dataframe::collect_all(
            {t.query(R"(cat == "POSIX")").lazy(),
             small.lazy().filter(col(0) > std::int64_t{1}),
             t.query(R"(cat == "STDIO")").lazy()}));
        CHECK(r[0].num_rows() == 30);
        CHECK(r[1].num_rows() == 2);
        CHECK(r[2].num_rows() == 20);
    }

    TEST_CASE("a join of two branches on one trace matches each side alone") {
        View t = base_viewer();
        View counts =
            t.group_by({GroupKey::name()}).agg({{AggOp::Count, "", "n"}});
        View durs = t.group_by({GroupKey::name()})
                        .agg({{AggOp::Sum, "dur", "sum_dur"}});
        DataFrame got =
            run(counts.join(durs, {"name"}).sort_by("name").collect());
        DataFrame want = run(run(counts.collect())
                                 .lazy()
                                 .join(run(durs.collect()).lazy(), {"name"})
                                 .sort_by("name")
                                 .collect());
        check_same_rows(got, want);
    }

    TEST_CASE("a concat of two row branches on one trace keeps every row") {
        View t = base_viewer();
        DataFrame got = run(
            t.query(R"(cat == "POSIX")")
                .select({"cat", "name"})
                .concat(t.query(R"(cat == "STDIO")").select({"cat", "name"}))
                .collect());
        CHECK(got.num_rows() == 50);
    }

    TEST_CASE("a session resolves its handles on execute") {
        View t = base_viewer();
        TraceSession s = t.session();
        Deferred<DataFrame> by_cat = s.collect(
            t.group_by({GroupKey::cat()}).agg({{AggOp::Count, "", "n"}}));
        Deferred<DataFrame> rows = s.collect(t.query(R"(cat == "STDIO")"));
        CHECK_THROWS(by_cat.get());
        run(s.execute());
        CHECK(by_cat.get().num_rows() == 2);
        CHECK(rows.get().num_rows() == 20);
        CHECK_THROWS(s.collect(t.lazy()));
    }
}

TEST_SUITE("View - planning reads no trace") {
    TEST_CASE("an aggregation's planned schema matches its result") {
        View t = base_viewer();
        std::vector<View> plans = {
            t.group_by({GroupKey::cat()}).agg({{AggOp::Count, "", "n"}}),
            t.group_by({GroupKey::name(), GroupKey::pid()})
                .agg({{AggOp::Sum, "dur", "s"}, {AggOp::Mean, "dur", "m"}}),
            t.time_bucket(1000)
                .group_by({GroupKey::cat()})
                .agg({{AggOp::Max, "dur", "mx"}}),
            t.time_range(0, 10000)
                .group_by({GroupKey::cat()})
                .agg({{AggOp::Busy, "dur", "b"}, {AggOp::Active, "dur", "a"}}),
            t.group_by({GroupKey::cat()})
                .agg({{AggOp::ArgMax, "name", "top", "dur"},
                      {AggOp::SetUnion, "name", "names"},
                      {AggOp::Count, "", "n"}}),
            t.group_by({GroupKey::name()})
                .agg({{AggOp::Pct, "dur", "p90", "", 0.9}}),
            t.agg({{AggOp::Count, "", "n"}}),
        };
        for (const View& p : plans) {
            dftracer::utils::dataframe::Schema planned = p.output_schema();
            DataFrame got = run(p.collect());
            REQUIRE(planned.fields.size() == got.names.size());
            for (std::size_t i = 0; i < got.names.size(); ++i) {
                CHECK(planned.fields[i].name == got.names[i]);
                INFO(got.names[i]);
                if (planned.fields[i].type.id !=
                    dftracer::utils::dataframe::TypeId::Unknown)
                    CHECK(planned.fields[i].type.id == got.columns[i].type());
            }
        }
    }

    TEST_CASE("explain on an aggregation does not scan") {
        auto polls = std::make_shared<int>(0);
        View t = base_viewer()
                     .cancel_when([polls] {
                         ++*polls;
                         return false;
                     })
                     .group_by({GroupKey::name()})
                     .agg({{AggOp::Count, "", "n"}});
        (void)t.explain();
        (void)t.schema();
        CHECK(*polls == 0);
        CHECK(run(t.collect()).num_rows() == 2);
        CHECK(*polls > 0);
    }
}

namespace {

// Events whose string arg `fname` is "/a" or "/b", so predicates on string
// args have something to select.
struct FnameTrace {
    TestEnvironment env{200};
    std::string gz;
    std::string idx;
    FnameTrace() {
        std::string pfw = env.get_dir() + "/fname.pfw";
        std::ofstream ofs(pfw);
        for (int i = 0; i < 20; ++i)
            ofs << R"({"ph":"X","name":"open","cat":"POSIX","pid":1,"tid":1,)"
                << R"("ts":)" << (1000 + i * 100) << R"(,"dur":)" << (5 + i)
                << R"(,"args":{"fname":")" << (i % 4 == 0 ? "/a" : "/b")
                << R"("}})" << "\n";
        ofs.close();
        gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        fs::remove(pfw);
        idx = determine_index_path(gz, "");
        StringSink sink;
        run(View::from_file(gz, idx).metadata(false).sink_json(sink));
    }
};

}  // namespace

TEST_SUITE("View - streamed batches") {
    TEST_CASE("row roots stream through one scan with their own filters") {
        View t = base_viewer();
        std::vector<LazyFrame> plans = {
            t.query(R"(cat == "POSIX")").select({"name", "dur"}).lazy(),
            t.phase(Phase::Events).query("dur > 25").lazy(),
            t.query(R"(cat == "STDIO")").lazy(),
            t.group_by({GroupKey::cat()})
                .agg({{AggOp::Count, "", "n"}})
                .sort_by("cat")
                .lazy(),
        };
        std::vector<DataFrame> together =
            run(dftracer::utils::dataframe::collect_all(plans));
        REQUIRE(together.size() == plans.size());
        for (std::size_t i = 0; i < plans.size(); ++i)
            CHECK(together[i].num_rows() == run(plans[i].collect()).num_rows());
        check_same_rows(together[3], run(plans[3].collect()));
    }

    TEST_CASE("a root that stops early does not stall the others") {
        View t = base_viewer();
        std::vector<DataFrame> r = run(dftracer::utils::dataframe::collect_all(
            {t.query(R"(cat == "POSIX")").head(2).lazy(),
             t.query(R"(cat == "STDIO")").lazy()}));
        CHECK(r[0].num_rows() == 2);
        CHECK(r[1].num_rows() == 20);
    }

    TEST_CASE("a branch predicate on a string arg selects its events") {
        FnameTrace f;
        View t = View::from_file(f.gz, f.idx).metadata(false);
        std::vector<DataFrame> r = run(dftracer::utils::dataframe::collect_all(
            {t.query(R"(fname == "/a")").lazy(),
             t.query(R"(fname == "/b")").lazy(),
             t.query(R"(fname == "/a")")
                 .group_by({GroupKey::name()})
                 .agg({{AggOp::Count, "", "n"}})
                 .lazy()}));
        CHECK(r[0].num_rows() == 5);
        CHECK(r[1].num_rows() == 15);
        REQUIRE(r[2].num_rows() == 1);
        CHECK(bnum(r[2], 0, "n") == 5);
    }
}

namespace {

std::int32_t column_at(const View& t, const std::string& name) {
    const std::vector<std::string> names = t.schema();
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) return static_cast<std::int32_t>(i);
    FAIL("no column " << name);
    return -1;
}

// The same plan run by the generic engine over the trace's rows in memory.
DataFrame engine_result(const View& t,
                        const std::function<LazyFrame(LazyFrame)>& shape) {
    return run(shape(run(t.collect()).lazy()).collect());
}

}  // namespace

TEST_SUITE("View - expression keys") {
    TEST_CASE("with_column then group_by is absorbed and matches the engine") {
        View t = base_viewer();
        const std::int32_t dur = column_at(t, "dur");
        auto shape = [&](auto lf) {
            return lf.with_column("big", col(dur) > std::int64_t{25})
                .group_by(std::vector<std::string>{"big"},
                          {{Agg::Count, "", "n"}, {Agg::Sum, "dur", "s"}})
                .sort_by("big");
        };
        View absorbed = shape(t);
        CHECK(absorbed.explain().find("group_by") == std::string::npos);
        CHECK(absorbed.explain().find("with_column") == std::string::npos);
        DataFrame got = run(absorbed.collect());
        DataFrame want = engine_result(
            t, [&](LazyFrame lf) { return shape(std::move(lf)); });
        check_same_rows(got, want);
        CHECK(got.column("big").type() == want.column("big").type());
    }

    TEST_CASE("an expression aggregate input is absorbed") {
        View t = base_viewer();
        const std::int32_t dur = column_at(t, "dur");
        auto shape = [&](auto lf) {
            return lf
                .with_column("dur2", col(dur) * dftracer::utils::dataframe::lit(
                                                    std::int64_t{2}))
                .group_by(std::vector<std::string>{"name"},
                          {{Agg::Sum, "dur2", "s2"}, {Agg::Sum, "dur", "s"}})
                .sort_by("name");
        };
        View absorbed = shape(t);
        CHECK(absorbed.explain().find("group_by") == std::string::npos);
        DataFrame got = run(absorbed.collect());
        check_same_rows(got, engine_result(t, [&](LazyFrame lf) {
                            return shape(std::move(lf));
                        }));
    }

    TEST_CASE("a computed-key rollup serves only the same expression") {
        const auto& sh = shared_trace();
        TestEnvironment env(10);
        const std::string roots = env.get_dir() + "/rollups";
        auto polls = std::make_shared<int>(0);
        View t = View::from_file(sh.gz, sh.idx)
                     .metadata(false)
                     .rollup_root(roots)
                     .cancel_when([polls] {
                         ++*polls;
                         return false;
                     });
        const std::int32_t dur = column_at(t, "dur");
        auto keyed = [&](std::int64_t cut) {
            return t.with_column("big", col(dur) > cut)
                .group_by(std::vector<std::string>{"big"},
                          {{Agg::Count, "", "n"}});
        };
        auto counts = [&](const DataFrame& f) {
            std::vector<std::string> out;
            for (std::int64_t i = 0; i < f.num_rows(); ++i)
                out.push_back(dftracer::utils::dataframe::cell_to_string(
                                  f.column("big"), i) +
                              "=" +
                              dftracer::utils::dataframe::cell_to_string(
                                  f.column("n"), i));
            std::sort(out.begin(), out.end());
            return out;
        };

        // Materialize the rollup through the absorbed view.
        const scan::ScanPlan absorbed =
            absorbed_source(keyed(25).lazy())->plan();
        const auto want25 =
            counts(run(scan::collect_frame(scan::materialize(absorbed, 0, 0))));

        *polls = 0;
        CHECK(counts(run(keyed(25).collect())) == want25);
        CHECK(*polls == 0);

        *polls = 0;
        const auto got40 = counts(run(keyed(40).collect()));
        CHECK(*polls > 0);
        CHECK(got40 != want25);
        CHECK(got40 == counts(run(keyed(40).collect())));
    }
}

TEST_SUITE("View - expression keys in a shared scan") {
    TEST_CASE("computed keys and inputs match collecting alone") {
        View t = base_viewer();
        const std::int32_t dur = column_at(t, "dur");
        std::vector<LazyFrame> plans = {
            t.with_column("big", col(dur) > std::int64_t{25})
                .group_by(std::vector<std::string>{"big"},
                          {{Agg::Count, "", "n"}})
                .sort_by("big")
                .lazy(),
            t.query(R"(cat == "POSIX")")
                .with_column("d2", col(dur) * dftracer::utils::dataframe::lit(
                                                  std::int64_t{2}))
                .group_by(std::vector<std::string>{"name"},
                          {{Agg::Sum, "d2", "s"}})
                .sort_by("name")
                .lazy(),
            t.query(R"(cat == "STDIO")").lazy(),
        };
        std::vector<DataFrame> together =
            run(dftracer::utils::dataframe::collect_all(plans));
        check_same_rows(together[0], run(plans[0].collect()));
        check_same_rows(together[1], run(plans[1].collect()));
        CHECK(together[2].num_rows() == 20);
    }
}

namespace {

// Two lanes of nested calls: an outer "step" holding a "read" that holds a
// "decode", then a sibling "write".
struct NestedTrace {
    TestEnvironment env{50};
    std::string gz;
    NestedTrace() {
        const std::string pfw = env.get_dir() + "/nested.pfw";
        std::ofstream ofs(pfw);
        auto ev = [&](const char* name, const char* cat, int tid, int ts,
                      int dur) {
            ofs << R"({"ph":"X","name":")" << name << R"(","cat":")" << cat
                << R"(","pid":1,"tid":)" << tid << R"(,"ts":)" << ts
                << R"(,"dur":)" << dur << R"(,"args":{}})"
                << "\n";
        };
        for (int tid = 1; tid <= 2; ++tid)
            for (int k = 0; k < 4; ++k) {
                const int t0 = 1000 + k * 1000;
                ev("step", "APP", tid, t0, 900);
                ev("read", "POSIX", tid, t0 + 10, 400);
                ev("decode", "APP", tid, t0 + 20, 100);
                ev("write", "POSIX", tid, t0 + 500, 300);
            }
        ofs.close();
        gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        fs::remove(pfw);
        StringSink sink;
        run(View::from_file(gz).metadata(false).sink_json(sink));
    }
};

const NestedTrace& nested_trace() {
    static NestedTrace t;
    return t;
}

scan::ScanPlan nested_plan() {
    return scan::metadata(scan::from_file(nested_trace().gz), false);
}

DataFrame scan_flamegraph(scan::ScanPlan p,
                          std::vector<std::string> group = {}) {
    return run(scan::flamegraph(std::move(p), {"pid", "tid"}, "ts", "dur",
                                "name", std::move(group)));
}

DataFrame scan_call_tree(scan::ScanPlan p) {
    return run(
        scan::call_tree(std::move(p), {"pid", "tid"}, "ts", "dur", "name"));
}

View nested_viewer() {
    return View::from_file(nested_trace().gz).metadata(false);
}

// The rows of `f` over `cols`, one string each, in sorted order: containment
// ids depend on scan order, so frames compare by content.
std::vector<std::string> rows_of(const DataFrame& f,
                                 const std::vector<std::string>& cols) {
    std::vector<std::string> out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r) {
        std::string row;
        for (const std::string& c : cols)
            row += dftracer::utils::dataframe::cell_to_string(f.column(c), r) +
                   "|";
        out.push_back(std::move(row));
    }
    std::sort(out.begin(), out.end());
    return out;
}

const std::vector<std::string> FLAME_COLS = {"name", "level", "total", "self",
                                             "count"};
const std::vector<std::string> TREE_COLS = {"pid", "tid",  "ts",
                                            "dur", "name", "level"};

}  // namespace

TEST_SUITE("View - trace terminals") {
    TEST_CASE("flamegraph and call_tree match the scan terminals") {
        const DataFrame fg = run(nested_viewer().flamegraph().collect());
        CHECK(rows_of(fg, FLAME_COLS) ==
              rows_of(scan_flamegraph(nested_plan()), FLAME_COLS));
        CHECK(fg.num_rows() == 5);
        const DataFrame ct = run(nested_viewer().call_tree().collect());
        CHECK(rows_of(ct, TREE_COLS) ==
              rows_of(scan_call_tree(nested_plan()), TREE_COLS));
        CHECK(ct.num_rows() == 32);
    }

    TEST_CASE("a flamegraph group roots one subtree per value") {
        const DataFrame got =
            run(nested_viewer()
                    .flamegraph({"pid", "tid"}, "ts", "dur", "name", {"tid"})
                    .collect());
        CHECK(rows_of(got, FLAME_COLS) ==
              rows_of(scan_flamegraph(nested_plan(), {"tid"}), FLAME_COLS));
    }

    TEST_CASE("a terminal's frame chains as a LazyFrame") {
        const DataFrame top =
            run(nested_viewer()
                    .flamegraph()
                    .filter(expr_cmp(CmpOp::Eq, col(3),
                                     dftracer::utils::dataframe::i64(1)))
                    .sort_by("total", true)
                    .head(1)
                    .collect());
        REQUIRE(top.num_rows() == 1);
        CHECK(bstr(top, 0, "name") == "step");
    }

    TEST_CASE("planning a terminal reads no trace") {
        auto polls = std::make_shared<int>(0);
        View t = nested_viewer().cancel_when([polls] {
            ++*polls;
            return false;
        });
        LazyFrame fg = t.flamegraph();
        LazyFrame ct = t.call_tree();
        CHECK(fg.schema() == std::vector<std::string>{"node_id", "parent",
                                                      "name", "level", "total",
                                                      "self", "count"});
        CHECK(ct.schema() == std::vector<std::string>{"pid", "tid", "ts", "dur",
                                                      "name", "level",
                                                      "parent_id"});
        CHECK(*polls == 0);
        CHECK(run(fg.collect()).names == fg.schema());
        CHECK(*polls > 0);
    }

    TEST_CASE("accepted prefixes reach the terminal") {
        View t = nested_viewer();
        const std::int32_t cat = column_at(t, "cat");
        auto same = [](const View& tv, scan::ScanPlan v) {
            CHECK(rows_of(run(tv.flamegraph().collect()), FLAME_COLS) ==
                  rows_of(scan_flamegraph(std::move(v)), FLAME_COLS));
        };
        same(t.query(R"(cat == "POSIX")"),
             scan::query(nested_plan(), R"(cat == "POSIX")"));
        same(t.filter(expr_cmp(CmpOp::Eq, col(cat), str("APP"))),
             scan::query(nested_plan(), R"(cat == "APP")"));
        same(t.phase(Phase::Events), scan::phase(nested_plan(), Phase::Events));
        same(t.time_range(0, 2500), scan::time_range(nested_plan(), 0, 2500));
        same(t.select({"name", "ts", "dur", "pid", "tid"}), nested_plan());
    }

    TEST_CASE("rejected prefixes name the first op the terminal cannot take") {
        View t = nested_viewer();
        CHECK_THROWS_WITH_AS(t.sort_by("ts").flamegraph(),
                             doctest::Contains("sort_by"), DFTUtilsException);
        CHECK_THROWS_WITH_AS(t.head(3).call_tree(), doctest::Contains("slice"),
                             DFTUtilsException);
        CHECK_THROWS_WITH_AS(
            t.with_column("d", col(column_at(t, "dur"))).flamegraph(),
            doctest::Contains("with_column"), DFTUtilsException);
        CHECK_THROWS_WITH_AS(t.group_by({GroupKey::name()})
                                 .agg({{AggOp::Count, "", "n"}})
                                 .flamegraph(),
                             doctest::Contains("aggregates"),
                             DFTUtilsException);
        CHECK_THROWS_WITH_AS(t.group_by(std::vector<std::string>{"name"},
                                        {{Agg::Count, "", "n"}})
                                 .containment(),
                             doctest::Contains("aggregates"),
                             DFTUtilsException);
        CHECK_THROWS_WITH_AS(t.aggregate_partial(),
                             doctest::Contains("group_by or agg"),
                             DFTUtilsException);
        StringSink sink;
        CHECK_THROWS_WITH_AS(t.sort_by("ts").sink_json(sink, LAZY),
                             doctest::Contains("sort_by"), DFTUtilsException);
    }

    TEST_CASE("containment returns both frames from one collect") {
        ContainmentResult both = run(nested_viewer().containment().collect());
        auto [ct, fg] = run(scan::containment(nested_plan(), {"pid", "tid"},
                                              "ts", "dur", "name", {}));
        CHECK(rows_of(both.call_tree, TREE_COLS) == rows_of(ct, TREE_COLS));
        CHECK(rows_of(both.flamegraph, FLAME_COLS) == rows_of(fg, FLAME_COLS));
    }

    TEST_CASE("partials merge to the collected result") {
        View t = nested_viewer();
        const std::string fp = run(t.flamegraph_partial().collect());
        const std::vector<std::string_view> fps{fp, fp};
        const DataFrame merged = View::merge_flamegraph_partials(fps);
        CHECK(rows_of(merged, {"name", "level"}) ==
              rows_of(run(t.flamegraph().collect()), {"name", "level"}));

        View agg =
            t.group_by({GroupKey::name()}).agg({{AggOp::Count, "", "n"}});
        const std::string ap = run(agg.aggregate_partial().collect());
        const std::vector<std::string_view> aps{ap};
        CHECK(rows_of(agg.merge_partials(aps), {"name", "n"}) ==
              rows_of(run(agg.collect()), {"name", "n"}));
    }

    TEST_CASE("sink_json writes the selected events") {
        StringSink eager;
        const ExportStats s =
            run(nested_viewer().query(R"(cat == "POSIX")").sink_json(eager));
        StringSink old;
        const ExportStats want = run(scan::export_json(
            scan::query(nested_plan(), R"(cat == "POSIX")"), old));
        CHECK(eager.lines() == old.lines());
        CHECK(eager.lines().size() == 16);
        CHECK(s.events_matched == want.events_matched);
    }

    TEST_CASE("one shared scan serves every terminal kind") {
        View t = nested_viewer();
        View agg =
            t.group_by({GroupKey::name()}).agg({{AggOp::Count, "", "n"}});
        StringSink sink;
        std::vector<LazyFrame> plans = {
            t.flamegraph(),
            t.call_tree(),
            t.query(R"(cat == "POSIX")").flamegraph(),
            t.flamegraph_partial().plans().front(),
            agg.aggregate_partial().plans().front(),
            t.query(R"(cat == "APP")").sink_json(sink, LAZY).plans().front(),
            agg.lazy()};
        std::vector<std::shared_ptr<const ViewSource>> members;
        for (const LazyFrame& lf : plans) {
            members.push_back(absorbed_source(lf));
            CHECK(members.back()->batch_key() == members.front()->batch_key());
        }
        ViewSource::Batch b = run(ViewSource::run_batch(members));
        CHECK(b.stats.events_scanned == 32);
        CHECK(rows_of(b.frames[0], FLAME_COLS) ==
              rows_of(run(plans[0].collect()), FLAME_COLS));
        CHECK(rows_of(b.frames[1], TREE_COLS) ==
              rows_of(run(plans[1].collect()), TREE_COLS));
        CHECK(rows_of(b.frames[2], FLAME_COLS) ==
              rows_of(scan_flamegraph(
                          scan::query(nested_plan(), R"(cat == "POSIX")")),
                      FLAME_COLS));
        const std::string fp = trace::views::detail::partial_of(b.frames[3]);
        const std::vector<std::string_view> fps{fp};
        CHECK(rows_of(View::merge_flamegraph_partials(fps), FLAME_COLS) ==
              rows_of(b.frames[0], FLAME_COLS));
        const std::string ap = trace::views::detail::partial_of(b.frames[4]);
        const std::vector<std::string_view> aps{ap};
        CHECK(rows_of(agg.merge_partials(aps), {"name", "n"}) ==
              rows_of(b.frames[6], {"name", "n"}));
        CHECK(trace::views::detail::stats_of(b.frames[5]).events_matched == 16);
        CHECK(sink.lines().size() == 16);
    }

    TEST_CASE("collect_all takes frames and lazy results in one call") {
        View t = nested_viewer();
        StringSink sink;
        auto [fg, both, stats, rows] =
            run(dftracer::utils::dataframe::collect_all(
                t.flamegraph(), t.containment(),
                t.query(R"(cat == "APP")").sink_json(sink, LAZY),
                t.query(R"(cat == "POSIX")")));
        CHECK(rows_of(fg, FLAME_COLS) == rows_of(both.flamegraph, FLAME_COLS));
        CHECK(both.call_tree.num_rows() == 32);
        CHECK(stats.events_matched == 16);
        CHECK(sink.lines().size() == 16);
        CHECK(rows.num_rows() == 16);
    }

    TEST_CASE("a session registers terminals and sinks") {
        View t = nested_viewer();
        TraceSession s = t.session();
        StringSink sink;
        Deferred<ContainmentResult> both = s.collect(t.containment());
        Deferred<ExportStats> stats =
            s.sink_json(t.query(R"(cat == "POSIX")"), sink);
        Deferred<DataFrame> fg = s.collect(t.flamegraph());
        Deferred<TypedResult> typed = s.collect(t.typed());
        CHECK_THROWS(both.get());
        run(s.execute());
        CHECK(both.get().call_tree.num_rows() == 32);
        CHECK(rows_of(fg.get(), FLAME_COLS) ==
              rows_of(both.get().flamegraph, FLAME_COLS));
        CHECK(stats.get().events_matched == 16);
        CHECK(sink.lines().size() == 16);
        const TypedResult want =
            run(scan::collect_typed(nested_plan(), 0, 4096));
        CHECK(typed.get().regular.num_rows() == want.regular.num_rows());
    }

    TEST_CASE("a batched containment branch honors phase") {
        TestEnvironment env(10);
        const std::string pfw = env.get_dir() + "/phased.pfw";
        {
            std::ofstream ofs(pfw);
            ofs << R"({"ph":"X","name":"step","cat":"APP","pid":1,"tid":1,"ts":100,"dur":50,"args":{}})"
                << "\n"
                << R"({"ph":"A","name":"folded","cat":"APP","pid":1,"tid":1,"ts":110,"dur":20,"args":{}})"
                << "\n";
        }
        const std::string gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        View t = View::from_file(gz).metadata(false);
        const DataFrame want = scan_flamegraph(scan::phase(
            scan::metadata(scan::from_file(gz), false), Phase::Events));
        auto [events, any] = run(dftracer::utils::dataframe::collect_all(
            t.phase(Phase::Events).flamegraph(), t.flamegraph()));
        CHECK(rows_of(events, FLAME_COLS) == rows_of(want, FLAME_COLS));
        CHECK(events.num_rows() == 2);
        CHECK(any.num_rows() == 3);
    }

    TEST_CASE("materialize persists a rollup that later reads serve") {
        TestEnvironment env(10);
        auto polls = std::make_shared<int>(0);
        View t = nested_viewer()
                     .rollup_root(env.get_dir() + "/rollups")
                     .cancel_when([polls] {
                         ++*polls;
                         return false;
                     })
                     .group_by({GroupKey::name()})
                     .agg({{AggOp::Count, "", "n"}});
        run(t.collect());
        CHECK(*polls > 0);
        TraceSession s = t.session();
        Deferred<ExportStats> built = s.materialize(t);
        run(s.execute());
        CHECK_NOTHROW(built.get());
        *polls = 0;
        CHECK(rows_of(run(t.collect()), {"name", "n"}) ==
              rows_of(run(scan::collect_frame(scan::agg(
                          scan::group_by(nested_plan(), {GroupKey::name()}),
                          {{AggOp::Count, "", "n"}}))),
                      {"name", "n"}));
        CHECK(*polls == 0);
    }

    TEST_CASE("a session branch runs alone or shares an unfiltered scan") {
        View t = nested_viewer();
        auto seen = std::make_shared<std::atomic<std::int64_t>>(0);
        auto counting = [seen](ViewSession& s) -> Deferred<std::int64_t> {
            return s.fold<std::int64_t>(
                dftracer::utils::query::parse_or_throw("dur >= 0"),
                [seen](std::int64_t& n, const json::JsonValue&,
                       std::string_view) {
                    ++n;
                    ++*seen;
                },
                [](std::int64_t&& a, std::int64_t&& b) { return a + b; });
        };
        CHECK(run(t.branch<std::int64_t>(counting).collect()) == 32);
        CHECK(run(t.query(R"(cat == "POSIX")")
                      .branch<std::int64_t>(counting)
                      .collect()) == 16);

        dftracer::utils::dataframe::LazyResult<std::int64_t> whole =
            t.branch<std::int64_t>(counting);
        CHECK(absorbed_source(whole.plans().front())->batch_key() ==
              absorbed_source(t.lazy())->batch_key());
        CHECK_FALSE(absorbed_source(t.query(R"(cat == "POSIX")")
                                        .branch<std::int64_t>(counting)
                                        .plans()
                                        .front())
                        ->batch_key());
        auto [n, rows] = run(dftracer::utils::dataframe::collect_all(
            whole, t.query(R"(cat == "APP")")));
        CHECK(n == 32);
        CHECK(rows.num_rows() == 16);
    }

    TEST_CASE("a plan owns a shared sink until it is dropped") {
        auto sink = std::make_shared<StringSink>();
        std::weak_ptr<StringSink> alive = sink;
        dftracer::utils::dataframe::LazyResult<ExportStats> lazy =
            nested_viewer().query(R"(cat == "APP")").sink_json(sink, LAZY);
        sink.reset();
        REQUIRE_FALSE(alive.expired());
        CHECK(run(lazy.collect()).events_matched == 16);
        CHECK(alive.lock()->lines().size() == 16);
    }

    TEST_CASE("sink_trace writes a trace the viewer reads back") {
        TestEnvironment env(10);
        TraceWriteOptions opts;
        opts.output_path = env.get_dir() + "/posix.pfw.gz";
        opts.compress = true;
        run(nested_viewer().query(R"(cat == "POSIX")").sink_trace(opts));
        CHECK(run(View::from_file(opts.output_path).metadata(false).collect())
                  .num_rows() == 16);
    }

    TEST_CASE("distributed rollup terminals work through the viewer") {
        TestEnvironment env(10);
        View agg = nested_viewer()
                       .rollup_root(env.get_dir() + "/rollups")
                       .group_by({GroupKey::name()})
                       .agg({{AggOp::Count, "", "n"}});
        CHECK_FALSE(agg.reconstruct_if_cached());
        const std::string p = run(agg.aggregate_partial().collect());
        run(agg.materialize_partials({p}));
        std::optional<DataFrame> cached = agg.reconstruct_if_cached();
        REQUIRE(cached);
        CHECK(rows_of(*cached, {"name", "n"}) ==
              rows_of(run(agg.collect()), {"name", "n"}));
        CHECK_THROWS_WITH_AS(nested_viewer().reconstruct_if_cached(),
                             doctest::Contains("group_by or agg"),
                             DFTUtilsException);
    }

    TEST_CASE("compare applies the baseline aggregation to the variant") {
        View base = nested_viewer()
                        .group_by({GroupKey::name()})
                        .agg({{AggOp::Count, "", "n"}});
        DataFrame self = run(base.compare(nested_viewer()).collect());
        CHECK(self.num_rows() == 4);
        for (std::int64_t i = 0; i < self.num_rows(); ++i) {
            CHECK(bnum(self, i, "l_n") == 8);
            CHECK(bnum(self, i, "delta_n") == 0);
        }
        DataFrame posix = run(
            base.compare(nested_viewer().query(R"(cat == "POSIX")")).collect());
        CHECK(rows_of(posix, {"name", "l_n", "r_n"}) ==
              std::vector<std::string>{"decode|8|0|", "read|8|8|", "step|8|0|",
                                       "write|8|8|"});
        CHECK_THROWS_WITH_AS(nested_viewer().compare(nested_viewer()),
                             doctest::Contains("group_by or agg"),
                             DFTUtilsException);
    }

    TEST_CASE("sink_counters writes the aggregation as counter events") {
        StringSink sink;
        run(nested_viewer()
                .group_by({GroupKey::name()})
                .agg({{AggOp::Count, "", "n"}})
                .sink_counters(sink));
        const std::vector<std::string> lines = sink.lines();
        CHECK(lines.size() == 4);
        CHECK(count_containing(lines, R"("ph":2)") == 4);
    }

    TEST_CASE("select on raw events reads fields the index does not list") {
        TestEnvironment env(10);
        const std::string pfw = env.get_dir() + "/args.pfw";
        {
            std::ofstream ofs(pfw);
            for (int i = 0; i < 4; ++i)
                ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
                    << 100 * i << R"(,"dur":10,"args":{"size":)" << i << "}}\n";
        }
        const std::string gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        View t = View::from_file(gz).metadata(false);
        DataFrame rows = run(t.select({"name", "size"}).collect());
        CHECK(rows.names == std::vector<std::string>{"name", "args.size"});
        CHECK(rows.num_rows() == 4);

        DataFrame agg = run(t.group_by({GroupKey::name()})
                                .agg({{AggOp::Count, "", "n"}})
                                .select({"n"})
                                .collect());
        CHECK(agg.names == std::vector<std::string>{"n"});
        CHECK(bnum(agg, 0, "n") == 4);
    }

    TEST_CASE("an Expr filter before a trace group_by filters the events") {
        View t = nested_viewer();
        const std::int32_t cat = column_at(t, "cat");
        DataFrame got =
            run(t.filter(expr_cmp(CmpOp::Eq, col(cat), str("POSIX")))
                    .group_by({GroupKey::name()})
                    .agg({{AggOp::Count, "", "n"}})
                    .collect());
        CHECK(rows_of(got, {"name", "n"}) ==
              std::vector<std::string>{"read|8|", "write|8|"});
    }

    TEST_CASE("a filter the scan cannot evaluate refuses a trace group_by") {
        View t = nested_viewer();
        const std::int32_t dur = column_at(t, "dur");
        const std::int32_t ts = column_at(t, "ts");
        View f = t.filter(expr_cmp(CmpOp::Gt, col(dur) + col(ts),
                                   dftracer::utils::dataframe::i64(1005)));
        CHECK(run(f.collect()).num_rows() > 0);
        CHECK_THROWS_WITH_AS(f.group_by({GroupKey::name()}),
                             doctest::Contains("filter"), DFTUtilsException);
        CHECK_NOTHROW(f.phase(Phase::Events));
    }
}

TEST_SUITE("View - low-level scans") {
    TEST_CASE("for_each_batch and map_batches see every selected event") {
        View t = nested_viewer().query(R"(cat == "POSIX")");
        std::atomic<std::int64_t> n{0};
        run(t.for_each_batch(
            [&](std::size_t, const std::vector<std::string_view>& events) {
                n += static_cast<std::int64_t>(events.size());
            },
            4));
        CHECK(n == 16);
        auto counted = run(t.map_batches<std::int64_t>(
            [](std::int64_t& acc, const std::vector<std::string_view>& ev) {
                acc += static_cast<std::int64_t>(ev.size());
            },
            [](std::int64_t&& a, std::int64_t&& b) { return a + b; }, 3));
        CHECK(counted.value == 16);
    }

    TEST_CASE("a head caps the low-level scans and the sinks") {
        View t = nested_viewer();
        std::atomic<std::int64_t> n{0};
        run(t.head(5).for_each_batch(
            [&](std::size_t, const std::vector<std::string_view>& events) {
                n += static_cast<std::int64_t>(events.size());
            },
            1));
        CHECK(n >= 5);
        StringSink sink;
        run(t.head(5).sink_json(sink));
        CHECK(sink.lines().size() == 5);
        StringSink skipped;
        run(t.slice(30, 10).sink_json(skipped));
        CHECK(skipped.lines().size() == 2);
        CHECK_THROWS_WITH_AS(
            run(t.slice(2, 5).for_each_batch(
                [](std::size_t, const std::vector<std::string_view>&) {}, 1)),
            doctest::Contains("head"), DFTUtilsException);
        CHECK_THROWS_WITH_AS(t.head(5).flamegraph(), doctest::Contains("slice"),
                             DFTUtilsException);
        dataframe::LazyResult<std::string> part =
            t.head(5).flamegraph_partial();
        CHECK_FALSE(run(part.collect()).empty());
        CHECK_THROWS_WITH_AS(t.slice(1, 5).flamegraph_partial(),
                             doctest::Contains("skips rows"),
                             DFTUtilsException);
    }

    TEST_CASE("config reads the trace description without a filter") {
        CHECK(nested_viewer().query(R"(cat == "POSIX")").config().size() ==
              scan::config(nested_plan()).size());
    }
}

TEST_SUITE("View - unindexed reads") {
    TEST_CASE("an unindexed multi-member trace reads every event once") {
        TestEnvironment env(10);
        const std::string gz = create_multimember_trace(env, 5000, 20000);
        const auto rows =
            run(View::from_file(gz, "").metadata(false).collect()).num_rows();
        CHECK(rows == 5000);
    }

    TEST_CASE(
        "a first-touch multi-member read builds its index and reads every "
        "event once") {
        TestEnvironment env(10);
        const std::string gz = create_multimember_trace(env, 5000, 20000);
        const std::string idx = determine_index_path(gz, "");
        CHECK(run(View::from_file(gz, idx).metadata(false).collect())
                  .num_rows() == 5000);
        CHECK(run(View::from_file(gz, idx).metadata(false).collect())
                  .num_rows() == 5000);
    }
}
