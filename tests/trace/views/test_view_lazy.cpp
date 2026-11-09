// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/cell_ops.h>
#include <dftracer/utils/dataframe/internal/lazy_plan.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/trace/views/view_source.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <algorithm>
#include <fstream>
#include <sstream>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

using namespace dftracer::utils::trace;

TEST_SUITE("trace scan - lazy collect") {
    TEST_CASE(
        "scan::collect() returns a LazyFrame matching scan::collect_frame()") {
        const auto& s = shared_trace();
        views::detail::scan::ScanPlan v = views::detail::scan::agg(
            views::detail::scan::group_by(
                views::detail::scan::from_file(dstd::String(s.gz),
                                               dstd::String(s.idx)),
                {GroupKey::cat()}),
            {{AggOp::Count, "", "n"}});

        dataframe::LazyFrame lz = views::detail::scan::collect(v);
        dataframe::DataFrame via_lazy = test_view_common::run(lz.collect());
        dataframe::DataFrame via_eager =
            test_view_common::run(views::detail::scan::collect_frame(v));

        REQUIRE(via_lazy.num_rows() == via_eager.num_rows());
        REQUIRE(via_lazy.num_rows() == 2);
        for (std::int64_t i = 0; i < via_lazy.num_rows(); ++i) {
            CHECK(bstr(via_lazy, i, "cat") == bstr(via_eager, i, "cat"));
            CHECK(bnum(via_lazy, i, "n") == bnum(via_eager, i, "n"));
        }
    }

    TEST_CASE("scan::collect() lazy plan composes filter + select") {
        const auto& s = shared_trace();
        views::detail::scan::ScanPlan base = views::detail::scan::from_file(
            dstd::String(s.gz), dstd::String(s.idx));

        dataframe::DataFrame all_events =
            test_view_common::run(views::detail::scan::collect_frame(base));

        dataframe::LazyFrame lz =
            views::detail::scan::collect(
                views::detail::scan::filter(
                    base, duql::parse_or_throw(R"(cat == "POSIX")")))
                .select({"cat", "name"});
        dataframe::DataFrame subset = test_view_common::run(lz.collect());

        CHECK(subset.num_rows() == 30);
        REQUIRE(bhas(subset, "cat"));
        REQUIRE(bhas(subset, "name"));
        CHECK(subset.columns.size() == 2);
        for (std::int64_t i = 0; i < subset.num_rows(); ++i)
            CHECK(bstr(subset, i, "cat") == "POSIX");
        CHECK(all_events.num_rows() == 50);
    }

    TEST_CASE("sorting and reversing a filter that matches nothing") {
        const auto& s = shared_trace();
        const auto none =
            View::from_file(dstd::String(s.gz), dstd::String(s.idx))
                .duql(R"(cat == "NOPE")")
                .select({"name", "dur"});
        CHECK(test_view_common::run(
                  none.sort_by_multi({"dur", "name"},
                                     dstd::Vector<bool>{true, false})
                      .slice(0, 10)
                      .collect())
                  .num_rows() == 0);
        CHECK(
            test_view_common::run(none.lazy().reverse().collect()).num_rows() ==
            0);
    }
}

// View: plan equality against the scan builders, source absorption and
// builder order.

namespace {

// Every ViewPlan field a scan reads, in a fixed order.
dstd::String canonical(const trace::views::detail::ViewPlan& p) {
    dftu_utils_test::TextOut o;
    for (const ViewFile& f : p.files) o << f.file_path << '|' << f.index_path;
    o << "\nquery=" << (p.query ? p.query->source() : "");
    o << "\nrange=";
    if (p.time_range) o << p.time_range->first << ',' << p.time_range->second;
    o << "\nphase=" << static_cast<int>(p.phase) << " all=" << p.all_records
      << " bucket=" << p.time_bucket_us << ',' << p.bucket_origin_us << ','
      << p.bucket_origin_min << " occ=" << p.occ_cell_us
      << " scale=" << p.time_scale;
    o << "\ngroup=";
    for (const GroupKey& k : p.group_by) {
        o << static_cast<int>(k.kind) << ':' << k.arg << ':'
          << static_cast<int>(k.transform);
        for (const auto& a : k.transform_args) o << ':' << a;
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
    for (const auto& s : p.select) o << s << ';';
    o << "\npage=" << p.limit << ',' << p.offset << " sort=" << p.sort_col
      << ',' << p.sort_desc << " topk=" << p.topk_col << ',' << p.topk_k << ','
      << p.topk_largest;
    o << "\nroots=" << p.rollup_root << '|' << p.views_root
      << " mat=" << p.materialize;
    return o.str();
}

// The optimized plan: the scan the source ends with, then the engine ops.
dstd::String optimized(const dataframe::LazyFrame& lf) {
    dataframe::LazyFrame opt = dataframe::detail::optimize_plan(lf);
    const auto* src = dynamic_cast<const ViewSource*>(
        dataframe::detail::plan_source(opt).get());
    REQUIRE(src != nullptr);
    dstd::String ops = opt.explain();
    ops = ops.substr(ops.find('\n') == dstd::String::npos ? ops.size()
                                                          : ops.find('\n'));
    return canonical(*src->plan()) + "\nops:" + dstd::String(ops);
}

void check_same_rows(const dataframe::DataFrame& a,
                     const dataframe::DataFrame& b) {
    REQUIRE(a.names == b.names);
    REQUIRE(a.num_rows() == b.num_rows());
    for (std::size_t c = 0; c < a.columns.size(); ++c)
        for (std::int64_t r = 0; r < a.num_rows(); ++r)
            CHECK(dftracer::utils::dataframe::cell_to_string(a.columns[c], r) ==
                  dftracer::utils::dataframe::cell_to_string(b.columns[c], r));
}

views::detail::scan::ScanPlan base_plan() {
    const auto& s = shared_trace();
    return views::detail::scan::from_file(dstd::String(s.gz),
                                          dstd::String(s.idx));
}

View base_viewer() {
    const auto& s = shared_trace();
    return View::from_file(dstd::String(s.gz), dstd::String(s.idx));
}

}  // namespace

TEST_SUITE("View - scan plan equality") {
    TEST_CASE("an aggregation with sort and limit") {
        views::detail::scan::ScanPlan v = views::detail::scan::limit(
            views::detail::scan::sort_by(
                views::detail::scan::agg(views::detail::scan::group_by(
                                             base_plan(), {GroupKey::cat()}),
                                         {{AggOp::Count, "", "n"}}),
                "n", true),
            1);
        View t = base_viewer()
                     .group_by({GroupKey::cat()})
                     .agg({{AggOp::Count, "", "n"}})
                     .sort_by("n", true)
                     .slice(0, 1);
        CHECK(optimized(views::detail::scan::collect(v)) ==
              optimized(t.lazy()));
        check_same_rows(
            test_view_common::run(views::detail::scan::collect(v).collect()),
            test_view_common::run(t.collect()));
    }

    TEST_CASE("an event filter, phase and time range before a group-by") {
        views::detail::scan::ScanPlan v = views::detail::scan::agg(
            views::detail::scan::group_by(
                views::detail::scan::time_range(
                    views::detail::scan::phase(
                        views::detail::scan::filter(
                            base_plan(),
                            duql::parse_or_throw(R"(cat == "POSIX")")),
                        Phase::Events),
                    1000, 3000),
                {GroupKey::name()}),
            {{AggOp::Sum, "dur", "sum_dur"}});
        View t = base_viewer()
                     .duql(R"(cat == "POSIX")")
                     .phase(Phase::Events)
                     .time_range(1000, 3000)
                     .group_by({GroupKey::name()})
                     .agg({{AggOp::Sum, "dur", "sum_dur"}});
        CHECK(optimized(views::detail::scan::collect(v)) ==
              optimized(t.lazy()));
        check_same_rows(
            test_view_common::run(views::detail::scan::collect(v).collect()),
            test_view_common::run(t.collect()));
    }

    TEST_CASE("a time bucket with top-k") {
        views::detail::scan::ScanPlan v = views::detail::scan::topk(
            views::detail::scan::agg(
                views::detail::scan::group_by(
                    views::detail::scan::time_bucket(base_plan(), 1000),
                    {GroupKey::cat()}),
                {{AggOp::Count, "", "n"}}),
            "n", 2);
        View t = base_viewer()
                     .time_bucket(1000)
                     .group_by({GroupKey::cat()})
                     .agg({{AggOp::Count, "", "n"}})
                     .topk("n", 2);
        CHECK(optimized(views::detail::scan::collect(v)) ==
              optimized(t.lazy()));
        check_same_rows(
            test_view_common::run(views::detail::scan::collect(v).collect()),
            test_view_common::run(t.collect()));
    }

    TEST_CASE("a row query with a select") {
        views::detail::scan::ScanPlan v = views::detail::scan::select(
            views::detail::scan::filter(
                base_plan(), duql::parse_or_throw(R"(cat == "STDIO")")),
            {"cat", "name"});
        View t =
            base_viewer().duql(R"(cat == "STDIO")").select({"cat", "name"});
        CHECK(optimized(views::detail::scan::collect(v)) ==
              optimized(t.lazy()));
        check_same_rows(
            test_view_common::run(views::detail::scan::collect(v).collect()),
            test_view_common::run(t.collect()));
    }

    TEST_CASE("a trace aggregation keeps its post-scan sort") {
        views::detail::scan::ScanPlan v = views::detail::scan::sort_by(
            views::detail::scan::agg(
                views::detail::scan::group_by(base_plan(), {GroupKey::cat()}),
                {{AggOp::Count, "", "n"}}),
            "cat");
        View t = base_viewer()
                     .group_by({GroupKey::cat()})
                     .agg({{AggOp::Count, "", "n"}})
                     .sort_by("cat");
        CHECK(optimized(views::detail::scan::collect(v)) ==
              optimized(t.lazy()));
        check_same_rows(
            test_view_common::run(views::detail::scan::collect(v).collect()),
            test_view_common::run(t.collect()));
    }
}

TEST_SUITE("View - source absorption") {
    TEST_CASE("an Expr filter is absorbed like the DSL filter") {
        View t = base_viewer();
        const dstd::Vector<dstd::String> names = t.schema();
        std::int32_t cat = -1;
        for (std::size_t i = 0; i < names.size(); ++i)
            if (names[i] == "cat") cat = static_cast<std::int32_t>(i);
        REQUIRE(cat >= 0);
        View by_expr = t.filter(dataframe::expr_cmp(dataframe::CmpOp::Eq,
                                                    dataframe::col(cat),
                                                    dataframe::str("POSIX")));
        CHECK(by_expr.explain().find("filter") == dstd::String::npos);
        dataframe::DataFrame a = test_view_common::run(by_expr.collect());
        dataframe::DataFrame b =
            test_view_common::run(t.duql(R"(cat == "POSIX")").collect());
        CHECK(a.num_rows() == 30);
        CHECK(a.num_rows() == b.num_rows());
    }

    TEST_CASE("a generic group-by on fixed fields is absorbed") {
        View t = base_viewer();
        View generic = t.group_by(dstd::Vector<dstd::String>{"name"},
                                  {{dataframe::Agg::Count, "", "n"},
                                   {dataframe::Agg::Sum, "dur", "sum_dur"}})
                           .sort_by("name");
        CHECK(generic.explain().find("group_by") == dstd::String::npos);
        View traced =
            t.group_by({GroupKey::name()})
                .agg({{AggOp::Count, "", "n"}, {AggOp::Sum, "dur", "sum_dur"}})
                .sort_by("name");
        check_same_rows(test_view_common::run(generic.collect()),
                        test_view_common::run(traced.collect()));
    }

    TEST_CASE("a generic group-by on cat keeps its raw case") {
        View g = base_viewer()
                     .group_by(dstd::Vector<dstd::String>{"cat"},
                               {{dataframe::Agg::Count, "", "n"}})
                     .sort_by("cat");
        CHECK(g.explain().find("group_by") == dstd::String::npos);
        dataframe::DataFrame r = test_view_common::run(g.collect());
        REQUIRE(r.num_rows() == 2);
        CHECK(bstr(r, 0, "cat") == "POSIX");
        CHECK(bstr(r, 1, "cat") == "STDIO");
    }

    TEST_CASE(
        "a group-by on any other field is absorbed as an expression key") {
        View t = base_viewer();
        View g = t.group_by(dstd::Vector<dstd::String>{"ph"},
                            {{dataframe::Agg::Count, "", "n"}});
        CHECK(g.explain().find("group_by") == dstd::String::npos);
        dataframe::DataFrame r = test_view_common::run(g.collect());
        REQUIRE(r.num_rows() == 1);
        CHECK(bnum(r, 0, "n") == 50);
    }

    TEST_CASE("an aggregated scan keeps a later Expr filter as an engine op") {
        View t = base_viewer()
                     .group_by({GroupKey::cat()})
                     .agg({{AggOp::Count, "", "n"}});
        View f = t.filter(dataframe::col(1) > std::int64_t{25});
        CHECK(f.explain().find("filter") != dstd::String::npos);
        dataframe::DataFrame r = test_view_common::run(f.collect());
        REQUIRE(r.num_rows() == 1);
        // The view's cat key is lowercased.
        CHECK(bstr(r, 0, "cat") == "posix");
    }
}

TEST_SUITE("View - builder order") {
    TEST_CASE("a trace builder after a non-filter op is refused") {
        View t = base_viewer().sort_by("ts");
        CHECK_THROWS_WITH_AS(t.phase(Phase::Events),
                             doctest::Contains("sort_by"),
                             dftracer::utils::common::DFTUtilsException);
    }

    TEST_CASE("a trace builder may follow an Expr filter") {
        View t = base_viewer().filter(dataframe::col(0) > std::int64_t{0});
        CHECK_NOTHROW(t.phase(Phase::Events));
    }

    TEST_CASE("the chain keeps the View type") {
        View t = base_viewer().select({"cat", "name"}).head(3);
        CHECK(test_view_common::run(t.collect()).num_rows() == 3);
    }
}

namespace {

dstd::SharedPtr<const ViewSource> absorbed_source(
    const dataframe::LazyFrame& lf) {
    return std::dynamic_pointer_cast<const ViewSource>(
        dataframe::detail::plan_source(dataframe::detail::optimize_plan(lf)));
}

dstd::Vector<dataframe::LazyFrame> mixed_plans() {
    View t = base_viewer();
    return {
        t.group_by({GroupKey::cat()})
            .agg({{AggOp::Count, "", "n"}})
            .sort_by("cat")
            .lazy(),
        t.duql(R"(cat == "POSIX")")
            .group_by({GroupKey::name()})
            .agg({{AggOp::Sum, "dur", "sum_dur"}})
            .lazy(),
        t.duql(R"(cat == "STDIO")").select({"cat", "name", "ts"}).lazy(),
        t.phase(Phase::Events).select({"name", "dur"}).sort_by("dur").lazy(),
    };
}

}  // namespace

TEST_SUITE("View - collect_all and sessions") {
    TEST_CASE("collect_all matches collecting each plan alone") {
        dstd::Vector<dataframe::LazyFrame> plans = mixed_plans();
        dstd::Vector<dataframe::DataFrame> together = test_view_common::run(
            dftracer::utils::dataframe::collect_all(plans));
        REQUIRE(together.size() == plans.size());
        for (std::size_t i = 0; i < plans.size(); ++i)
            check_same_rows(together[i],
                            test_view_common::run(plans[i].collect()));
    }

    TEST_CASE("plans on one trace base share the batch key") {
        dstd::Vector<dataframe::LazyFrame> plans = mixed_plans();
        dstd::Optional<dstd::String> key =
            absorbed_source(plans[0])->batch_key();
        REQUIRE(key);
        for (const dataframe::LazyFrame& lf : plans)
            CHECK(absorbed_source(lf)->batch_key() == key);
    }

    TEST_CASE("a batch scans the trace once for every branch") {
        dstd::Vector<dstd::SharedPtr<const ViewSource>> members;
        for (const dataframe::LazyFrame& lf : mixed_plans())
            members.push_back(absorbed_source(lf));
        ViewSource::Batch b =
            test_view_common::run(ViewSource::run_batch(members));
        REQUIRE(b.frames.size() == members.size());
        CHECK(b.stats.events_scanned == 50);
    }

    TEST_CASE("a different time range is a different base") {
        View t = base_viewer();
        View early = t.time_range(0, 3000);
        CHECK(absorbed_source(t.lazy())->batch_key() !=
              absorbed_source(early.lazy())->batch_key());
        dstd::Vector<dataframe::DataFrame> r = test_view_common::run(
            dftracer::utils::dataframe::collect_all({t.lazy(), early.lazy()}));
        CHECK(r[0].num_rows() == 50);
        CHECK(r[1].num_rows() ==
              test_view_common::run(early.collect()).num_rows());
    }

    TEST_CASE("collect_all mixes trace and in-memory plans") {
        dataframe::DataFrame small;
        dstd::Vector<std::int64_t> a{1, 2, 3};
        small.names = {"a"};
        small.columns.push_back(
            dftracer::utils::dataframe::Series::flat_i64(a.data(), 3));
        View t = base_viewer();
        dstd::Vector<dataframe::DataFrame> r =
            test_view_common::run(dftracer::utils::dataframe::collect_all(
                {t.duql(R"(cat == "POSIX")").lazy(),
                 small.lazy().filter(dataframe::col(0) > std::int64_t{1}),
                 t.duql(R"(cat == "STDIO")").lazy()}));
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
        dataframe::DataFrame got = test_view_common::run(
            counts.join(durs, {"name"}).sort_by("name").collect());
        dataframe::DataFrame want = test_view_common::run(
            test_view_common::run(counts.collect())
                .lazy()
                .join(test_view_common::run(durs.collect()).lazy(), {"name"})
                .sort_by("name")
                .collect());
        check_same_rows(got, want);
    }

    TEST_CASE("a concat of two row branches on one trace keeps every row") {
        View t = base_viewer();
        dataframe::DataFrame got = test_view_common::run(
            t.duql(R"(cat == "POSIX")")
                .select({"cat", "name"})
                .concat(t.duql(R"(cat == "STDIO")").select({"cat", "name"}))
                .collect());
        CHECK(got.num_rows() == 50);
    }

    TEST_CASE("a session resolves its handles on execute") {
        View t = base_viewer();
        TraceSession s = t.session();
        Deferred<dataframe::DataFrame> by_cat = s.collect(
            t.group_by({GroupKey::cat()}).agg({{AggOp::Count, "", "n"}}));
        Deferred<dataframe::DataFrame> rows =
            s.collect(t.duql(R"(cat == "STDIO")"));
        CHECK_THROWS(by_cat.get());
        test_view_common::run(s.execute());
        CHECK(by_cat.get().num_rows() == 2);
        CHECK(rows.get().num_rows() == 20);
        CHECK_THROWS(s.collect(t.lazy()));
    }
}

TEST_SUITE("View - planning reads no trace") {
    TEST_CASE("an aggregation's planned schema matches its result") {
        View t = base_viewer();
        dstd::Vector<View> plans = {
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
            dataframe::DataFrame got = test_view_common::run(p.collect());
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
        auto polls = dstd::make_shared<int>(0);
        View t = base_viewer()
                     .cancel_when([polls] {
                         ++*polls;
                         return false;
                     })
                     .group_by({GroupKey::name()})
                     .agg({{AggOp::Count, "", "n"}});
        t.explain();
        t.schema();
        CHECK(*polls == 0);
        CHECK(test_view_common::run(t.collect()).num_rows() == 2);
        CHECK(*polls > 0);
    }
}

namespace {

// Events whose string arg `fname` is "/a" or "/b", so predicates on string
// args have something to select.
struct FnameTrace {
    TestEnvironment env{200};
    dstd::String gz;
    dstd::String idx;
    FnameTrace() {
        dstd::String pfw = env.dir() + "/fname.pfw";
        dftu_utils_test::FileOut ofs(pfw);
        for (int i = 0; i < 20; ++i)
            ofs << R"({"ph":"X","name":"open","cat":"POSIX","pid":1,"tid":1,)"
                << R"("ts":)" << (1000 + i * 100) << R"(,"dur":)" << (5 + i)
                << R"(,"args":{"fname":")" << (i % 4 == 0 ? "/a" : "/b")
                << R"("}})" << "\n";
        ofs.close();
        gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        dstd::fs::remove(pfw).discard();
        idx = dstd::String(determine_index_path(gz, ""));
        StringSink sink;
        test_view_common::run(
            View::from_file(dstd::String(gz), dstd::String(idx))
                .sink_json(sink));
    }
};

}  // namespace

TEST_SUITE("View - streamed batches") {
    TEST_CASE("row roots stream through one scan with their own filters") {
        View t = base_viewer();
        dstd::Vector<dataframe::LazyFrame> plans = {
            t.duql(R"(cat == "POSIX")").select({"name", "dur"}).lazy(),
            t.phase(Phase::Events).duql("dur > 25").lazy(),
            t.duql(R"(cat == "STDIO")").lazy(),
            t.group_by({GroupKey::cat()})
                .agg({{AggOp::Count, "", "n"}})
                .sort_by("cat")
                .lazy(),
        };
        dstd::Vector<dataframe::DataFrame> together = test_view_common::run(
            dftracer::utils::dataframe::collect_all(plans));
        REQUIRE(together.size() == plans.size());
        for (std::size_t i = 0; i < plans.size(); ++i)
            CHECK(together[i].num_rows() ==
                  test_view_common::run(plans[i].collect()).num_rows());
        check_same_rows(together[3], test_view_common::run(plans[3].collect()));
    }

    TEST_CASE("a root that stops early does not stall the others") {
        View t = base_viewer();
        dstd::Vector<dataframe::DataFrame> r =
            test_view_common::run(dftracer::utils::dataframe::collect_all(
                {t.duql(R"(cat == "POSIX")").head(2).lazy(),
                 t.duql(R"(cat == "STDIO")").lazy()}));
        CHECK(r[0].num_rows() == 2);
        CHECK(r[1].num_rows() == 20);
    }

    TEST_CASE("a branch predicate on a string arg selects its events") {
        FnameTrace f;
        View t = View::from_file(dstd::String(f.gz), dstd::String(f.idx));
        dstd::Vector<dataframe::DataFrame> r =
            test_view_common::run(dftracer::utils::dataframe::collect_all(
                {t.duql(R"(fname == "/a")").lazy(),
                 t.duql(R"(fname == "/b")").lazy(),
                 t.duql(R"(fname == "/a")")
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

std::int32_t column_at(const View& t, const dstd::String& name) {
    const dstd::Vector<dstd::String> names = t.schema();
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) return static_cast<std::int32_t>(i);
    FAIL("no column " << name);
    return -1;
}

// The same plan run by the generic engine over the trace's rows in memory.
dataframe::DataFrame engine_result(
    const View& t,
    const dstd::Function<dataframe::LazyFrame(dataframe::LazyFrame)>& shape) {
    return test_view_common::run(
        shape(test_view_common::run(t.collect()).lazy()).collect());
}

}  // namespace

TEST_SUITE("View - expression keys") {
    TEST_CASE("with_column then group_by is absorbed and matches the engine") {
        View t = base_viewer();
        const std::int32_t dur = column_at(t, "dur");
        auto shape = [&](auto lf) {
            return lf.with_column("big", dataframe::col(dur) > std::int64_t{25})
                .group_by(dstd::Vector<dstd::String>{"big"},
                          {{dataframe::Agg::Count, "", "n"},
                           {dataframe::Agg::Sum, "dur", "s"}})
                .sort_by("big");
        };
        View absorbed = shape(t);
        CHECK(absorbed.explain().find("group_by") == dstd::String::npos);
        CHECK(absorbed.explain().find("with_column") == dstd::String::npos);
        dataframe::DataFrame got = test_view_common::run(absorbed.collect());
        dataframe::DataFrame want = engine_result(
            t, [&](dataframe::LazyFrame lf) { return shape(std::move(lf)); });
        check_same_rows(got, want);
        CHECK(got.column("big").type() == want.column("big").type());
    }

    TEST_CASE("an expression aggregate input is absorbed") {
        View t = base_viewer();
        const std::int32_t dur = column_at(t, "dur");
        auto shape = [&](auto lf) {
            return lf
                .with_column("dur2", dataframe::col(dur) *
                                         dftracer::utils::dataframe::lit(
                                             std::int64_t{2}))
                .group_by(dstd::Vector<dstd::String>{"name"},
                          {{dataframe::Agg::Sum, "dur2", "s2"},
                           {dataframe::Agg::Sum, "dur", "s"}})
                .sort_by("name");
        };
        View absorbed = shape(t);
        CHECK(absorbed.explain().find("group_by") == dstd::String::npos);
        dataframe::DataFrame got = test_view_common::run(absorbed.collect());
        check_same_rows(got, engine_result(t, [&](dataframe::LazyFrame lf) {
                            return shape(std::move(lf));
                        }));
    }

    TEST_CASE("a computed-key rollup serves only the same expression") {
        const auto& sh = shared_trace();
        TestEnvironment env(10);
        const dstd::String roots = env.dir() + "/rollups";
        auto polls = dstd::make_shared<int>(0);
        View t = View::from_file(dstd::String(sh.gz), dstd::String(sh.idx))
                     .rollup_root(dstd::String(roots))
                     .cancel_when([polls] {
                         ++*polls;
                         return false;
                     });
        const std::int32_t dur = column_at(t, "dur");
        auto keyed = [&](std::int64_t cut) {
            return t.with_column("big", dataframe::col(dur) > cut)
                .group_by(dstd::Vector<dstd::String>{"big"},
                          {{dataframe::Agg::Count, "", "n"}});
        };
        auto counts = [&](const dataframe::DataFrame& f) {
            dstd::Vector<dstd::String> out;
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
        const views::detail::scan::ScanPlan absorbed =
            absorbed_source(keyed(25).lazy())->plan();
        const auto want25 =
            counts(test_view_common::run(views::detail::scan::collect_frame(
                views::detail::scan::materialize(absorbed, 0, 0))));

        *polls = 0;
        CHECK(counts(test_view_common::run(keyed(25).collect())) == want25);
        CHECK(*polls == 0);

        *polls = 0;
        const auto got40 = counts(test_view_common::run(keyed(40).collect()));
        CHECK(*polls > 0);
        CHECK(got40 != want25);
        CHECK(got40 == counts(test_view_common::run(keyed(40).collect())));
    }
}

TEST_SUITE("View - expression keys in a shared scan") {
    TEST_CASE("computed keys and inputs match collecting alone") {
        View t = base_viewer();
        const std::int32_t dur = column_at(t, "dur");
        dstd::Vector<dataframe::LazyFrame> plans = {
            t.with_column("big", dataframe::col(dur) > std::int64_t{25})
                .group_by(dstd::Vector<dstd::String>{"big"},
                          {{dataframe::Agg::Count, "", "n"}})
                .sort_by("big")
                .lazy(),
            t.duql(R"(cat == "POSIX")")
                .with_column(
                    "d2", dataframe::col(dur) *
                              dftracer::utils::dataframe::lit(std::int64_t{2}))
                .group_by(dstd::Vector<dstd::String>{"name"},
                          {{dataframe::Agg::Sum, "d2", "s"}})
                .sort_by("name")
                .lazy(),
            t.duql(R"(cat == "STDIO")").lazy(),
        };
        dstd::Vector<dataframe::DataFrame> together = test_view_common::run(
            dftracer::utils::dataframe::collect_all(plans));
        check_same_rows(together[0], test_view_common::run(plans[0].collect()));
        check_same_rows(together[1], test_view_common::run(plans[1].collect()));
        CHECK(together[2].num_rows() == 20);
    }
}

namespace {

// Two lanes of nested calls: an outer "step" holding a "read" that holds a
// "decode", then a sibling "write".
struct NestedTrace {
    TestEnvironment env{50};
    dstd::String gz;
    NestedTrace() {
        const dstd::String pfw = env.dir() + "/nested.pfw";
        dftu_utils_test::FileOut ofs(pfw);
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
        dstd::fs::remove(pfw).discard();
        StringSink sink;
        test_view_common::run(
            View::from_file(dstd::String(gz)).sink_json(sink));
    }
};

const NestedTrace& nested_trace() {
    static NestedTrace t;
    return t;
}

views::detail::scan::ScanPlan nested_plan() {
    return views::detail::scan::from_file(dstd::String(nested_trace().gz));
}

dataframe::DataFrame scan_flamegraph(views::detail::scan::ScanPlan p,
                                     dstd::Vector<dstd::String> group = {}) {
    return test_view_common::run(views::detail::scan::flamegraph(
        std::move(p), {"pid", "tid"}, "ts", "dur", "name", std::move(group)));
}

dataframe::DataFrame scan_call_tree(views::detail::scan::ScanPlan p) {
    return test_view_common::run(views::detail::scan::call_tree(
        std::move(p), {"pid", "tid"}, "ts", "dur", "name"));
}

View nested_viewer() {
    return View::from_file(dstd::String(nested_trace().gz));
}

// The rows of `f` over `cols`, one string each, in sorted order: containment
// ids depend on scan order, so frames compare by content.
dstd::Vector<dstd::String> rows_of(const dataframe::DataFrame& f,
                                   const dstd::Vector<dstd::String>& cols) {
    dstd::Vector<dstd::String> out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r) {
        dstd::String row;
        for (const dstd::String& c : cols)
            row += dftracer::utils::dataframe::cell_to_string(f.column(c), r) +
                   "|";
        out.push_back(std::move(row));
    }
    std::sort(out.begin(), out.end());
    return out;
}

const dstd::Vector<dstd::String> FLAME_COLS = {"name", "level", "total", "self",
                                               "count"};
const dstd::Vector<dstd::String> TREE_COLS = {"pid", "tid",  "ts",
                                              "dur", "name", "level"};

}  // namespace

TEST_SUITE("View - trace terminals") {
    TEST_CASE("flamegraph and call_tree match the scan terminals") {
        const dataframe::DataFrame fg =
            test_view_common::run(nested_viewer().flamegraph().collect());
        CHECK(rows_of(fg, FLAME_COLS) ==
              rows_of(scan_flamegraph(nested_plan()), FLAME_COLS));
        CHECK(fg.num_rows() == 5);
        const dataframe::DataFrame ct =
            test_view_common::run(nested_viewer().call_tree().collect());
        CHECK(rows_of(ct, TREE_COLS) ==
              rows_of(scan_call_tree(nested_plan()), TREE_COLS));
        CHECK(ct.num_rows() == 32);
    }

    TEST_CASE("a flamegraph group roots one subtree per value") {
        const dataframe::DataFrame got = test_view_common::run(
            nested_viewer()
                .flamegraph({"pid", "tid"}, "ts", "dur", "name", {"tid"})
                .collect());
        CHECK(rows_of(got, FLAME_COLS) ==
              rows_of(scan_flamegraph(nested_plan(), {"tid"}), FLAME_COLS));
    }

    TEST_CASE("a terminal's frame chains as a LazyFrame") {
        const dataframe::DataFrame top = test_view_common::run(
            nested_viewer()
                .flamegraph()
                .filter(dataframe::expr_cmp(dataframe::CmpOp::Eq,
                                            dataframe::col(3),
                                            dftracer::utils::dataframe::i64(1)))
                .sort_by("total", true)
                .head(1)
                .collect());
        REQUIRE(top.num_rows() == 1);
        CHECK(bstr(top, 0, "name") == "step");
    }

    TEST_CASE("planning a terminal reads no trace") {
        auto polls = dstd::make_shared<int>(0);
        View t = nested_viewer().cancel_when([polls] {
            ++*polls;
            return false;
        });
        dataframe::LazyFrame fg = t.flamegraph();
        dataframe::LazyFrame ct = t.call_tree();
        CHECK(fg.schema() ==
              dstd::Vector<dstd::String>{"node_id", "parent", "name", "level",
                                         "total", "self", "count"});
        CHECK(ct.schema() == dstd::Vector<dstd::String>{"pid", "tid", "ts",
                                                        "dur", "name", "level",
                                                        "parent_id"});
        CHECK(*polls == 0);
        CHECK(test_view_common::run(fg.collect()).names == fg.schema());
        CHECK(*polls > 0);
    }

    TEST_CASE("accepted prefixes reach the terminal") {
        View t = nested_viewer();
        const std::int32_t cat = column_at(t, "cat");
        auto same = [](const View& tv, views::detail::scan::ScanPlan v) {
            CHECK(rows_of(test_view_common::run(tv.flamegraph().collect()),
                          FLAME_COLS) ==
                  rows_of(scan_flamegraph(std::move(v)), FLAME_COLS));
        };
        same(t.duql(R"(cat == "POSIX")"),
             views::detail::scan::filter(
                 nested_plan(), duql::parse_or_throw(R"(cat == "POSIX")")));
        same(t.filter(dataframe::expr_cmp(dataframe::CmpOp::Eq,
                                          dataframe::col(cat),
                                          dataframe::str("APP"))),
             views::detail::scan::filter(
                 nested_plan(), duql::parse_or_throw(R"(cat == "APP")")));
        same(t.phase(Phase::Events),
             views::detail::scan::phase(nested_plan(), Phase::Events));
        same(t.time_range(0, 2500),
             views::detail::scan::time_range(nested_plan(), 0, 2500));
        same(t.select({"name", "ts", "dur", "pid", "tid"}), nested_plan());
    }

    TEST_CASE("rejected prefixes name the first op the terminal cannot take") {
        View t = nested_viewer();
        CHECK_THROWS_WITH_AS(t.sort_by("ts").flamegraph(),
                             doctest::Contains("sort_by"),
                             dftracer::utils::common::DFTUtilsException);
        CHECK_THROWS_WITH_AS(t.head(3).call_tree(), doctest::Contains("slice"),
                             dftracer::utils::common::DFTUtilsException);
        CHECK_THROWS_WITH_AS(
            t.with_column("d", dataframe::col(column_at(t, "dur")))
                .flamegraph(),
            doctest::Contains("with_column"),
            dftracer::utils::common::DFTUtilsException);
        CHECK_THROWS_WITH_AS(t.group_by({GroupKey::name()})
                                 .agg({{AggOp::Count, "", "n"}})
                                 .flamegraph(),
                             doctest::Contains("aggregates"),
                             dftracer::utils::common::DFTUtilsException);
        CHECK_THROWS_WITH_AS(t.group_by(dstd::Vector<dstd::String>{"name"},
                                        {{dataframe::Agg::Count, "", "n"}})
                                 .containment(),
                             doctest::Contains("aggregates"),
                             dftracer::utils::common::DFTUtilsException);
        CHECK_THROWS_WITH_AS(t.aggregate_partial(),
                             doctest::Contains("group_by or agg"),
                             dftracer::utils::common::DFTUtilsException);
        StringSink sink;
        CHECK_THROWS_WITH_AS(t.sort_by("ts").sink_json(sink, LAZY),
                             doctest::Contains("sort_by"),
                             dftracer::utils::common::DFTUtilsException);
    }

    TEST_CASE("containment returns both frames from one collect") {
        ContainmentResult both =
            test_view_common::run(nested_viewer().containment().collect());
        auto [ct, fg] = test_view_common::run(views::detail::scan::containment(
            nested_plan(), {"pid", "tid"}, "ts", "dur", "name", {}));
        CHECK(rows_of(both.call_tree, TREE_COLS) == rows_of(ct, TREE_COLS));
        CHECK(rows_of(both.flamegraph, FLAME_COLS) == rows_of(fg, FLAME_COLS));
    }

    TEST_CASE("partials merge to the collected result") {
        View t = nested_viewer();
        const dstd::String fp =
            test_view_common::run(t.flamegraph_partial().collect());
        const dstd::Vector<dstd::StringView> fps{fp, fp};
        const dataframe::DataFrame merged =
            View::merge_flamegraph_partials(fps);
        CHECK(rows_of(merged, {"name", "level"}) ==
              rows_of(test_view_common::run(t.flamegraph().collect()),
                      {"name", "level"}));

        View agg =
            t.group_by({GroupKey::name()}).agg({{AggOp::Count, "", "n"}});
        const dstd::String ap =
            test_view_common::run(agg.aggregate_partial().collect());
        const dstd::Vector<dstd::StringView> aps{ap};
        CHECK(rows_of(agg.merge_partials(aps), {"name", "n"}) ==
              rows_of(test_view_common::run(agg.collect()), {"name", "n"}));
    }

    TEST_CASE("sink_json writes the selected events") {
        StringSink eager;
        const ExportStats s = test_view_common::run(
            nested_viewer().duql(R"(cat == "POSIX")").sink_json(eager));
        StringSink old;
        const ExportStats want =
            test_view_common::run(views::detail::scan::export_json(
                views::detail::scan::filter(
                    nested_plan(), duql::parse_or_throw(R"(cat == "POSIX")")),
                old));
        CHECK(eager.lines() == old.lines());
        CHECK(eager.lines().size() == 16);
        CHECK(s.events_matched == want.events_matched);
    }

    TEST_CASE("one shared scan serves every terminal kind") {
        View t = nested_viewer();
        View agg =
            t.group_by({GroupKey::name()}).agg({{AggOp::Count, "", "n"}});
        StringSink sink;
        dstd::Vector<dataframe::LazyFrame> plans = {
            t.flamegraph(),
            t.call_tree(),
            t.duql(R"(cat == "POSIX")").flamegraph(),
            t.flamegraph_partial().plans().front(),
            agg.aggregate_partial().plans().front(),
            t.duql(R"(cat == "APP")").sink_json(sink, LAZY).plans().front(),
            agg.lazy()};
        dstd::Vector<dstd::SharedPtr<const ViewSource>> members;
        for (const dataframe::LazyFrame& lf : plans) {
            members.push_back(absorbed_source(lf));
            CHECK(members.back()->batch_key() == members.front()->batch_key());
        }
        ViewSource::Batch b =
            test_view_common::run(ViewSource::run_batch(members));
        CHECK(b.stats.events_scanned == 32);
        CHECK(rows_of(b.frames[0], FLAME_COLS) ==
              rows_of(test_view_common::run(plans[0].collect()), FLAME_COLS));
        CHECK(rows_of(b.frames[1], TREE_COLS) ==
              rows_of(test_view_common::run(plans[1].collect()), TREE_COLS));
        CHECK(rows_of(b.frames[2], FLAME_COLS) ==
              rows_of(scan_flamegraph(views::detail::scan::filter(
                          nested_plan(),
                          duql::parse_or_throw(R"(cat == "POSIX")"))),
                      FLAME_COLS));
        const dstd::String fp = trace::views::detail::partial_of(b.frames[3]);
        const dstd::Vector<dstd::StringView> fps{fp};
        CHECK(rows_of(View::merge_flamegraph_partials(fps), FLAME_COLS) ==
              rows_of(b.frames[0], FLAME_COLS));
        const dstd::String ap = trace::views::detail::partial_of(b.frames[4]);
        const dstd::Vector<dstd::StringView> aps{ap};
        CHECK(rows_of(agg.merge_partials(aps), {"name", "n"}) ==
              rows_of(b.frames[6], {"name", "n"}));
        CHECK(trace::views::detail::stats_of(b.frames[5]).events_matched == 16);
        CHECK(sink.lines().size() == 16);
    }

    TEST_CASE("collect_all takes frames and lazy results in one call") {
        View t = nested_viewer();
        StringSink sink;
        auto [fg, both, stats, rows] =
            test_view_common::run(dftracer::utils::dataframe::collect_all(
                t.flamegraph(), t.containment(),
                t.duql(R"(cat == "APP")").sink_json(sink, LAZY),
                t.duql(R"(cat == "POSIX")")));
        CHECK(rows_of(fg, FLAME_COLS) == rows_of(both.flamegraph, FLAME_COLS));
        CHECK(both.call_tree.num_rows() == 32);
        CHECK(stats.events_matched == 16);
        CHECK(sink.lines().size() == 16);
        CHECK(rows.num_rows() == 16);
    }

    TEST_CASE("collect_typed leaves dur null where the record has none") {
        TestEnvironment env(10);
        const dstd::String pfw = env.dir() + "/dur_gap.pfw";
        {
            dftu_utils_test::FileOut ofs(pfw);
            ofs << R"({"ph":"X","name":"a","cat":"APP","pid":1,"tid":1,"ts":100,"dur":50,"args":{}})"
                << "\n"
                << R"({"ph":"X","name":"b","cat":"APP","pid":1,"tid":2,"ts":150,"args":{}})"
                << "\n";
        }
        const dstd::String gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        dstd::fs::remove(pfw).discard();
        const TypedResult t =
            test_view_common::run(views::detail::scan::collect_typed(
                views::detail::scan::from_file(dstd::String(gz)), 0, 4096));
        REQUIRE(t.regular.num_rows() == 2);
        const dataframe::Series& dur =
            t.regular.columns[static_cast<std::size_t>(bcol(t.regular, "dur"))];
        REQUIRE(dur.length() == 2);
        CHECK_FALSE(dur.is_null(0));
        CHECK(dur.is_null(1));
        CHECK(bnum(t.regular, 0, "dur") == 50.0);
    }

    TEST_CASE("a session registers terminals and sinks") {
        View t = nested_viewer();
        TraceSession s = t.session();
        StringSink sink;
        Deferred<ContainmentResult> both = s.collect(t.containment());
        Deferred<ExportStats> stats =
            s.sink_json(t.duql(R"(cat == "POSIX")"), sink);
        Deferred<dataframe::DataFrame> fg = s.collect(t.flamegraph());
        Deferred<TypedResult> typed = s.collect(t.typed());
        CHECK_THROWS(both.get());
        test_view_common::run(s.execute());
        CHECK(both.get().call_tree.num_rows() == 32);
        CHECK(rows_of(fg.get(), FLAME_COLS) ==
              rows_of(both.get().flamegraph, FLAME_COLS));
        CHECK(stats.get().events_matched == 16);
        CHECK(sink.lines().size() == 16);
        const TypedResult want = test_view_common::run(
            views::detail::scan::collect_typed(nested_plan(), 0, 4096));
        CHECK(typed.get().regular.num_rows() == want.regular.num_rows());
    }

    TEST_CASE("a batched containment branch honors phase") {
        TestEnvironment env(10);
        const dstd::String pfw = env.dir() + "/phased.pfw";
        {
            dftu_utils_test::FileOut ofs(pfw);
            ofs << R"({"ph":"X","name":"step","cat":"APP","pid":1,"tid":1,"ts":100,"dur":50,"args":{}})"
                << "\n"
                << R"({"ph":"A","name":"folded","cat":"APP","pid":1,"tid":1,"ts":110,"dur":20,"args":{}})"
                << "\n";
        }
        const dstd::String gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        View t = View::from_file(dstd::String(gz));
        const dataframe::DataFrame want =
            scan_flamegraph(views::detail::scan::phase(
                views::detail::scan::from_file(dstd::String(gz)),
                Phase::Events));
        auto [events, any] =
            test_view_common::run(dftracer::utils::dataframe::collect_all(
                t.phase(Phase::Events).flamegraph(), t.flamegraph()));
        CHECK(rows_of(events, FLAME_COLS) == rows_of(want, FLAME_COLS));
        CHECK(events.num_rows() == 2);
        CHECK(any.num_rows() == 3);
    }

    TEST_CASE("materialize persists a rollup that later reads serve") {
        TestEnvironment env(10);
        auto polls = dstd::make_shared<int>(0);
        View t = nested_viewer()
                     .rollup_root(dstd::String(env.dir() + "/rollups"))
                     .cancel_when([polls] {
                         ++*polls;
                         return false;
                     })
                     .group_by({GroupKey::name()})
                     .agg({{AggOp::Count, "", "n"}});
        test_view_common::run(t.collect());
        CHECK(*polls > 0);
        TraceSession s = t.session();
        Deferred<ExportStats> built = s.materialize(t);
        test_view_common::run(s.execute());
        CHECK_NOTHROW(built.get());
        *polls = 0;
        CHECK(rows_of(test_view_common::run(t.collect()), {"name", "n"}) ==
              rows_of(test_view_common::run(views::detail::scan::collect_frame(
                          views::detail::scan::agg(
                              views::detail::scan::group_by(nested_plan(),
                                                            {GroupKey::name()}),
                              {{AggOp::Count, "", "n"}}))),
                      {"name", "n"}));
        CHECK(*polls == 0);
    }

    TEST_CASE("a session branch runs alone or shares an unfiltered scan") {
        View t = nested_viewer();
        auto seen = dstd::make_shared<std::atomic<std::int64_t>>(0);
        auto counting = [seen](ViewSession& s) -> Deferred<std::int64_t> {
            return s.fold<std::int64_t>(
                dftracer::utils::duql::parse_or_throw("dur >= 0"),
                [seen](std::int64_t& n, json::Value&, dstd::StringView) {
                    ++n;
                    ++*seen;
                },
                [](std::int64_t&& a, std::int64_t&& b) { return a + b; });
        };
        CHECK(test_view_common::run(
                  t.branch<std::int64_t>(counting).collect()) == 32);
        CHECK(test_view_common::run(t.duql(R"(cat == "POSIX")")
                                        .branch<std::int64_t>(counting)
                                        .collect()) == 16);

        dftracer::utils::dataframe::LazyResult<std::int64_t> whole =
            t.branch<std::int64_t>(counting);
        CHECK(absorbed_source(whole.plans().front())->batch_key() ==
              absorbed_source(t.lazy())->batch_key());
        CHECK_FALSE(absorbed_source(t.duql(R"(cat == "POSIX")")
                                        .branch<std::int64_t>(counting)
                                        .plans()
                                        .front())
                        ->batch_key());
        auto [n, rows] =
            test_view_common::run(dftracer::utils::dataframe::collect_all(
                whole, t.duql(R"(cat == "APP")")));
        CHECK(n == 32);
        CHECK(rows.num_rows() == 16);
    }

    TEST_CASE("a plan owns a shared sink until it is dropped") {
        auto sink = dstd::make_shared<StringSink>();
        dstd::WeakPtr<StringSink> alive = sink;
        dftracer::utils::dataframe::LazyResult<ExportStats> lazy =
            nested_viewer().duql(R"(cat == "APP")").sink_json(sink, LAZY);
        sink.reset();
        REQUIRE_FALSE(alive.expired());
        CHECK(test_view_common::run(lazy.collect()).events_matched == 16);
        CHECK(alive.lock()->lines().size() == 16);
    }

    TEST_CASE("sink_trace writes a trace the viewer reads back") {
        TestEnvironment env(10);
        TraceWriteOptions opts;
        opts.output_path = env.dir() + "/posix.pfw.gz";
        opts.compress = true;
        test_view_common::run(
            nested_viewer().duql(R"(cat == "POSIX")").sink_trace(opts));
        CHECK(test_view_common::run(View::from_file(opts.output_path).collect())
                  .num_rows() == 16);
    }

    TEST_CASE("distributed rollup terminals work through the viewer") {
        TestEnvironment env(10);
        View agg = nested_viewer()
                       .rollup_root(dstd::String(env.dir() + "/rollups"))
                       .group_by({GroupKey::name()})
                       .agg({{AggOp::Count, "", "n"}});
        CHECK_FALSE(agg.reconstruct_if_cached());
        const dstd::String p =
            test_view_common::run(agg.aggregate_partial().collect());
        test_view_common::run(agg.materialize_partials({p}));
        dstd::Optional<dataframe::DataFrame> cached =
            agg.reconstruct_if_cached();
        REQUIRE(cached);
        CHECK(rows_of(*cached, {"name", "n"}) ==
              rows_of(test_view_common::run(agg.collect()), {"name", "n"}));
        CHECK_THROWS_WITH_AS(nested_viewer().reconstruct_if_cached(),
                             doctest::Contains("group_by or agg"),
                             dftracer::utils::common::DFTUtilsException);
    }

    TEST_CASE("compare applies the baseline aggregation to the variant") {
        View base = nested_viewer()
                        .group_by({GroupKey::name()})
                        .agg({{AggOp::Count, "", "n"}});
        dataframe::DataFrame self =
            test_view_common::run(base.compare(nested_viewer()).collect());
        CHECK(self.num_rows() == 4);
        for (std::int64_t i = 0; i < self.num_rows(); ++i) {
            CHECK(bnum(self, i, "l_n") == 8);
            CHECK(bnum(self, i, "delta_n") == 0);
        }
        dataframe::DataFrame posix = test_view_common::run(
            base.compare(nested_viewer().duql(R"(cat == "POSIX")")).collect());
        CHECK(rows_of(posix, {"name", "l_n", "r_n"}) ==
              dstd::Vector<dstd::String>{"decode|8|0|", "read|8|8|",
                                         "step|8|0|", "write|8|8|"});
        CHECK_THROWS_WITH_AS(nested_viewer().compare(nested_viewer()),
                             doctest::Contains("group_by or agg"),
                             dftracer::utils::common::DFTUtilsException);
    }

    TEST_CASE("sink_counters writes the aggregation as counter events") {
        StringSink sink;
        test_view_common::run(nested_viewer()
                                  .group_by({GroupKey::name()})
                                  .agg({{AggOp::Count, "", "n"}})
                                  .sink_counters(sink));
        const dstd::Vector<dstd::String> lines = sink.lines();
        CHECK(lines.size() == 4);
        CHECK(count_containing(lines, R"("ph":2)") == 4);
    }

    TEST_CASE("select on raw events reads fields the index does not list") {
        TestEnvironment env(10);
        const dstd::String pfw = env.dir() + "/args.pfw";
        {
            dftu_utils_test::FileOut ofs(pfw);
            for (int i = 0; i < 4; ++i)
                ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
                    << 100 * i << R"(,"dur":10,"args":{"size":)" << i << "}}\n";
        }
        const dstd::String gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        View t = View::from_file(dstd::String(gz));
        dataframe::DataFrame rows =
            test_view_common::run(t.select({"name", "size"}).collect());
        CHECK(rows.names == dstd::Vector<dstd::String>{"name", "args.size"});
        CHECK(rows.num_rows() == 4);

        dataframe::DataFrame agg =
            test_view_common::run(t.group_by({GroupKey::name()})
                                      .agg({{AggOp::Count, "", "n"}})
                                      .select({"n"})
                                      .collect());
        CHECK(agg.names == dstd::Vector<dstd::String>{"n"});
        CHECK(bnum(agg, 0, "n") == 4);
    }

    TEST_CASE("an Expr filter before a trace group_by filters the events") {
        View t = nested_viewer();
        const std::int32_t cat = column_at(t, "cat");
        dataframe::DataFrame got = test_view_common::run(
            t.filter(dataframe::expr_cmp(dataframe::CmpOp::Eq,
                                         dataframe::col(cat),
                                         dataframe::str("POSIX")))
                .group_by({GroupKey::name()})
                .agg({{AggOp::Count, "", "n"}})
                .collect());
        CHECK(rows_of(got, {"name", "n"}) ==
              dstd::Vector<dstd::String>{"read|8|", "write|8|"});
    }

    TEST_CASE("a filter the scan cannot evaluate refuses a trace group_by") {
        View t = nested_viewer();
        const std::int32_t dur = column_at(t, "dur");
        const std::int32_t ts = column_at(t, "ts");
        View f = t.filter(dataframe::expr_cmp(
            dataframe::CmpOp::Gt, dataframe::col(dur) + dataframe::col(ts),
            dftracer::utils::dataframe::i64(1005)));
        CHECK(test_view_common::run(f.collect()).num_rows() > 0);
        CHECK_THROWS_WITH_AS(f.group_by({GroupKey::name()}),
                             doctest::Contains("filter"),
                             dftracer::utils::common::DFTUtilsException);
        CHECK_NOTHROW(f.phase(Phase::Events));
    }
}

TEST_SUITE("View - low-level scans") {
    TEST_CASE("for_each_batch and map_batches see every selected event") {
        View t = nested_viewer().duql(R"(cat == "POSIX")");
        std::atomic<std::int64_t> n{0};
        test_view_common::run(t.for_each_batch(
            [&](std::size_t, const dstd::Vector<dstd::StringView>& events) {
                n += static_cast<std::int64_t>(events.size());
            },
            4));
        CHECK(n == 16);
        auto counted = test_view_common::run(t.map_batches<std::int64_t>(
            [](std::int64_t& acc, const dstd::Vector<dstd::StringView>& ev) {
                acc += static_cast<std::int64_t>(ev.size());
            },
            [](std::int64_t&& a, std::int64_t&& b) { return a + b; }, 3));
        CHECK(counted.value == 16);
    }

    TEST_CASE("a head caps the low-level scans and the sinks") {
        View t = nested_viewer();
        std::atomic<std::int64_t> n{0};
        test_view_common::run(t.head(5).for_each_batch(
            [&](std::size_t, const dstd::Vector<dstd::StringView>& events) {
                n += static_cast<std::int64_t>(events.size());
            },
            1));
        CHECK(n >= 5);
        StringSink sink;
        test_view_common::run(t.head(5).sink_json(sink));
        CHECK(sink.lines().size() == 5);
        StringSink skipped;
        test_view_common::run(t.slice(30, 10).sink_json(skipped));
        CHECK(skipped.lines().size() == 2);
        CHECK_THROWS_WITH_AS(
            test_view_common::run(t.slice(2, 5).for_each_batch(
                [](std::size_t, const dstd::Vector<dstd::StringView>&) {}, 1)),
            doctest::Contains("head"),
            dftracer::utils::common::DFTUtilsException);
        CHECK_THROWS_WITH_AS(t.head(5).flamegraph(), doctest::Contains("slice"),
                             dftracer::utils::common::DFTUtilsException);
        auto part = t.head(5).flamegraph_partial();
        CHECK_FALSE(test_view_common::run(part.collect()).empty());
        CHECK_THROWS_WITH_AS(t.slice(1, 5).flamegraph_partial(),
                             doctest::Contains("skips rows"),
                             dftracer::utils::common::DFTUtilsException);
    }

    TEST_CASE("config reads the trace description without a filter") {
        CHECK(nested_viewer().duql(R"(cat == "POSIX")").config().size() ==
              views::detail::scan::config(nested_plan()).size());
    }
}

TEST_SUITE("View - unindexed reads") {
    TEST_CASE("an unindexed multi-member trace reads every event once") {
        TestEnvironment env(10);
        const dstd::String gz = create_multimember_trace(env, 5000, 20000);
        const auto rows = test_view_common::run(
                              View::from_file(dstd::String(gz), "").collect())
                              .num_rows();
        CHECK(rows == 5000);
    }

    TEST_CASE(
        "a first-touch multi-member read builds its index and reads every "
        "event once") {
        TestEnvironment env(10);
        const dstd::String gz = create_multimember_trace(env, 5000, 20000);
        const dstd::String idx = dstd::String(determine_index_path(gz, ""));
        CHECK(
            test_view_common::run(
                View::from_file(dstd::String(gz), dstd::String(idx)).collect())
                .num_rows() == 5000);
        CHECK(
            test_view_common::run(
                View::from_file(dstd::String(gz), dstd::String(idx)).collect())
                .num_rows() == 5000);
    }
}

namespace {

dstd::String write_args_trace(TestEnvironment& env) {
    const dstd::String pfw = env.dir() + "/args.pfw";
    {
        dftu_utils_test::FileOut ofs(pfw);
        ofs << R"({"ph":"X","name":"op","cat":"POSIX","pid":1,"tid":1,"ts":1,"dur":10,"args":{"x":1,"s":"a"}})"
            << "\n"
            << R"({"ph":"X","name":"op","cat":"POSIX","pid":1,"tid":1,"ts":2,"dur":10,"args":{"x":null}})"
            << "\n"
            << R"({"ph":"X","name":"op","cat":"POSIX","pid":1,"tid":1,"ts":3,"dur":10,"args":{"x":3}})"
            << "\n";
    }
    const dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dstd::fs::remove(pfw).discard();
    return gz;
}

dstd::Vector<dstd::String> cells(const dataframe::DataFrame& df,
                                 const dstd::String& name) {
    const dftracer::utils::dataframe::Series c = df.column(name).materialize();
    dstd::Vector<dstd::String> out;
    for (std::int64_t i = 0; i < c.length(); ++i)
        out.push_back(c.is_null(i)
                          ? "null"
                          : dftracer::utils::dataframe::cell_to_string(c, i));
    return out;
}

}  // namespace

TEST_SUITE("View - computed columns over a trace scan") {
    TEST_CASE("select after with_column reads the computed columns") {
        TestEnvironment env(10);
        const dstd::String gz = write_args_trace(env);
        View v = View::from_file(
            dstd::String(gz),
            dstd::String(dstd::String(determine_index_path(gz, ""))));
        View a =
            v.with_column("z", dataframe::expr_lit(std::int64_t{1}))
                .with_column("t",
                             dataframe::expr_lit_null(dataframe::TypeId::Int64))
                .select({"t", "z"});
        dataframe::DataFrame out = test_view_common::run(a.collect());
        REQUIRE(out.names == dstd::Vector<dstd::String>{"t", "z"});
        CHECK(cells(out, "t") ==
              dstd::Vector<dstd::String>{"null", "null", "null"});
        CHECK(cells(out, "z") == dstd::Vector<dstd::String>{"1", "1", "1"});
    }

    TEST_CASE("a filter on a row index keeps its rows over many morsels") {
        TestEnvironment env(10);
        const dstd::String gz = create_multimember_trace(env, 20000, 1 << 16);
        View big =
            View::from_file(dstd::String(gz), determine_index_path(gz, ""));
        dataframe::LazyFrame lf = big.lazy().with_row_index("r");
        const std::int32_t r = 0;
        REQUIRE(lf.schema()[0] == "r");
        dataframe::DataFrame out = test_view_common::run(
            lf.filter(dataframe::expr_cmp_expr(
                          dataframe::CmpOp::Lt, dataframe::expr_col(r),
                          dataframe::expr_lit(std::int64_t{5})))
                .collect());
        CHECK(out.num_rows() == 5);
    }

    TEST_CASE("first and last over an ordered multi-member scan") {
        TestEnvironment env(10);
        for (std::size_t member : {std::size_t{2048}, std::size_t{20000}}) {
            CAPTURE(member);
            const dstd::String gz = create_multimember_trace(env, 3000, member);
            dataframe::LazyFrame lf = views::detail::scan::collect(
                views::detail::scan::ordered(views::detail::scan::from_file(
                    dstd::String(gz), determine_index_path(gz, ""))));
            const dstd::Vector<dstd::String> names = lf.schema();
            const auto at = [&](const char* n) {
                return static_cast<std::int32_t>(
                    std::find(names.begin(), names.end(), n) - names.begin());
            };
            dataframe::LazyFrame grouped =
                lf.with_column("k", dataframe::expr_arith(
                                        dataframe::ArithOp::Mod,
                                        dataframe::expr_col(at("dur")),
                                        dataframe::expr_lit(std::int64_t{7})))
                    .group_by(dstd::Vector<dstd::String>{"k"},
                              {{dataframe::Agg::First, "ts", "f"},
                               {dataframe::Agg::Last, "ts", "l"}})
                    .sort_by("k");
            dstd::Vector<dstd::String> want_f, want_l;
            for (std::int64_t k = 0; k < 7; ++k) {
                std::int64_t lo = -1, hi = -1;
                for (std::int64_t i = 0; i < 3000; ++i)
                    if ((10 + i) % 7 == k) {
                        if (lo < 0) lo = 1000 + i * 100;
                        hi = 1000 + i * 100;
                    }
                want_f.push_back(dstd::to_string(lo));
                want_l.push_back(dstd::to_string(hi));
            }
            for (std::size_t workers : {1, 2, 8}) {
                CAPTURE(workers);
                dftracer::utils::Runtime rt(workers);
                dataframe::DataFrame out =
                    dftu_utils_test::ok(rt.submit(grouped.collect()).get());
                CHECK(cells(out, "f") == want_f);
                CHECK(cells(out, "l") == want_l);
            }
        }
    }
}

TEST_SUITE("View - aggregations over a nullable arg") {
    TEST_CASE("absorbed or not, the aggregation skips nulls") {
        TestEnvironment env(10);
        const dstd::String gz = write_args_trace(env);
        dataframe::LazyFrame lf = views::detail::scan::collect(
            views::detail::scan::ordered(views::detail::scan::select(
                views::detail::scan::from_file(dstd::String(gz),
                                               determine_index_path(gz, "")),
                {"name", "cat", "pid", "tid", "ts", "dur", "ph", "x"})));
        const std::int32_t x =
            static_cast<std::int32_t>(lf.schema().size()) - 1;
        const dataframe::LazyFrame in =
            lf.with_column("__in", dataframe::expr_col(x));

        const dataframe::LazyFrame absorbed =
            in.group_by(dstd::Vector<dstd::String>{},
                        {{dataframe::Agg::Count, "", "n"},
                         {dataframe::Agg::Sum, "__in", "s"},
                         {dataframe::Agg::Mean, "__in", "m"}});
        CHECK(dataframe::detail::optimize_plan(absorbed).explain().find(
                  "group_by") == dstd::String::npos);
        const dataframe::LazyFrame engine =
            in.group_by(dstd::Vector<dstd::String>{},
                        {{dataframe::Agg::Count, "", "n"},
                         {dataframe::Agg::CountValid, "__in", "c"},
                         {dataframe::Agg::Sum, "__in", "s"},
                         {dataframe::Agg::Mean, "__in", "m"}});
        CHECK(dataframe::detail::optimize_plan(engine).explain().find(
                  "group_by") != dstd::String::npos);
        for (const dataframe::LazyFrame& plan : {absorbed, engine}) {
            dataframe::DataFrame out = test_view_common::run(plan.collect());
            CHECK(cells(out, "n") == dstd::Vector<dstd::String>{"3"});
            CHECK(cells(out, "s") == dstd::Vector<dstd::String>{"4"});
            CHECK(out.column("m").materialize().data<double>()[0] ==
                  doctest::Approx(2.0));
            if (out.column_index("c") >= 0)
                CHECK(cells(out, "c") == dstd::Vector<dstd::String>{"2"});
        }
    }
}

TEST_SUITE("View - aggregate types over an untyped arg") {
    TEST_CASE("sum, min and max keep the runtime integer type") {
        TestEnvironment env(10);
        const dstd::Vector<dstd::String> cols{"name", "cat", "pid", "tid",
                                              "ts",   "dur", "ph",  "x"};
        const dstd::Vector<dataframe::GroupAgg> typed{
            {dataframe::Agg::Sum, "args.x", "s"},
            {dataframe::Agg::Min, "args.x", "lo"},
            {dataframe::Agg::Max, "args.x", "hi"}};
        // CountValid keeps the group-by in the engine.
        dstd::Vector<dataframe::GroupAgg> engine = typed;
        engine.push_back({dataframe::Agg::CountValid, "args.x", "c"});

        const dstd::String absent_pfw = env.dir() + "/absent.pfw";
        {
            dftu_utils_test::FileOut ofs(absent_pfw);
            ofs << R"({"ph":"X","name":"op","cat":"POSIX","pid":1,"tid":1,"ts":1,"dur":10,"args":{"x":1}})"
                << "\n"
                << R"({"ph":"X","name":"op","cat":"POSIX","pid":1,"tid":1,"ts":2,"dur":10,"args":{"y":2}})"
                << "\n"
                << R"({"ph":"X","name":"op","cat":"POSIX","pid":1,"tid":1,"ts":3,"dur":10,"args":{"x":3}})"
                << "\n";
        }
        const dstd::String absent = absent_pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(absent_pfw, absent);
        dstd::fs::remove(absent_pfw).discard();

        for (const dstd::String& gz : {write_args_trace(env), absent}) {
            CAPTURE(gz);
            const dstd::String idx = determine_index_path(gz, "");
            View v = View::from_file(dstd::String(gz), idx);
            const dataframe::LazyFrame ordered = views::detail::scan::collect(
                views::detail::scan::ordered(views::detail::scan::select(
                    views::detail::scan::from_file(dstd::String(gz), idx),
                    cols)));
            for (const dataframe::LazyFrame& lf :
                 {v.select(cols).lazy(), ordered,
                  v.select(cols).duql("derive q = 1").lazy()})
                for (const dstd::Vector<dataframe::GroupAgg>& aggs :
                     {typed, engine})
                    for (std::int64_t morsel :
                         {std::int64_t{1}, std::int64_t{0}}) {
                        const dataframe::LazyFrame agg =
                            lf.group_by(dstd::Vector<dstd::String>{}, aggs);
                        CAPTURE(
                            dataframe::detail::optimize_plan(agg).explain());
                        CAPTURE(morsel);
                        // The declared type is Unknown until the index
                        // has typed the arg; the aggregate follows it.
                        dataframe::TypeId declared = dataframe::TypeId::Unknown;
                        for (const auto& f : lf.output_schema().fields)
                            if (f.name == "args.x") declared = f.type.id;
                        CHECK((declared == dataframe::TypeId::Unknown ||
                               declared == dataframe::TypeId::Int64));
                        for (const auto& f : agg.output_schema().fields)
                            if (f.name != "c") CHECK(f.type.id == declared);
                        dataframe::DataFrame out =
                            test_view_common::run(agg.collect(morsel));
                        for (const char* c : {"s", "lo", "hi"})
                            CHECK(out.column(c).type() ==
                                  dataframe::TypeId::Int64);
                        CHECK(cells(out, "s") ==
                              dstd::Vector<dstd::String>{"4"});
                        CHECK(cells(out, "lo") ==
                              dstd::Vector<dstd::String>{"1"});
                        CHECK(cells(out, "hi") ==
                              dstd::Vector<dstd::String>{"3"});
                    }
        }
    }
}

namespace {

// Four lines; the second has a bad atom (`tru`) in a part no filter reads.
// The fold path reads only what a branch asks for, so it keeps that line.
struct BadPartTrace {
    TestEnvironment env{51};
    dstd::String gz;
    BadPartTrace() {
        const dstd::String pfw = env.dir() + "/badpart.pfw";
        dftu_utils_test::FileOut ofs(pfw);
        auto ev = [&](int ts, const char* tail) {
            ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,)"
                << R"("ts":)" << ts << R"(,"dur":10,"args":{})" << tail
                << "}\n";
        };
        ev(1, "");
        ev(2, R"(,"zz":tru)");
        ev(3, "");
        ev(4, "");
        ofs.close();
        gz = pfw + ".gz";
        dftu_utils_test::compress_file_to_gzip(pfw, gz);
        dstd::fs::remove(pfw).discard();
    }
};

}  // namespace

TEST_SUITE("View - malformed lines") {
    TEST_CASE(
        "an export writes a line whose bad part nothing reads, verbatim") {
        BadPartTrace trace;
        StringSink sink;
        test_view_common::run(
            View::from_file(dstd::String(trace.gz)).sink_json(sink));
        const auto lines = sink.lines();
        REQUIRE(lines.size() == 4);
        std::size_t bad = 0;
        for (const dstd::String& line : lines)
            if (line.find("tru}") != dstd::String::npos) {
                ++bad;
                CHECK(
                    line ==
                    R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,)"
                    R"("ts":2,"dur":10,"args":{},"zz":tru})");
            }
        CHECK(bad == 1);
    }
}

namespace {

constexpr int BATCH_FILES = 3;
constexpr int BATCH_MEMBERS = 6;
constexpr int BATCH_PER_MEMBER = 400;

// BATCH_FILES gzip files of BATCH_MEMBERS members each, so a scan has many
// units to hand out of order.
View batch_viewer(const dstd::String& dir,
                  dstd::SharedPtr<ScanCounter> counter) {
    dstd::Vector<ViewFile> files;
    const char* names[] = {"read", "write", "open", "close"};
    for (int f = 0; f < BATCH_FILES; ++f) {
        dstd::Vector<std::uint8_t> bytes;
        for (int m = 0; m < BATCH_MEMBERS; ++m) {
            dstd::String text;
            if (f == 0 && m == 0)
                text +=
                    R"({"ph":"M","name":"process_name","pid":1,"tid":0,"args":{"name":"app"}})"
                    "\n";
            for (int i = 0; i < BATCH_PER_MEMBER; ++i) {
                const int n = (f * BATCH_MEMBERS + m) * BATCH_PER_MEMBER + i;
                text += dstd::String("{\"ph\":\"X\",\"name\":\"") +
                        names[n % 4] + "\",\"cat\":\"" +
                        (n % 4 < 2 ? "POSIX" : "STDIO") +
                        "\",\"pid\":" + dstd::to_string(1 + n % 5) +
                        ",\"tid\":1,\"ts\":" + dstd::to_string(1000 + n * 7) +
                        ",\"dur\":" + dstd::to_string(1 + (n * 37) % 500) +
                        ",\"args\":{\"size\":" + dstd::to_string(n) + "}}\n";
            }
            utilities::fileio::compress::GzipMemberCompressor c;
            dstd::Vector<std::uint8_t> buf(c.bound(text.size()));
            auto len =
                c.compress(text.data(), text.size(), buf.data(), buf.size());
            REQUIRE(len.has_value());
            bytes.insert(bytes.end(), buf.begin(), buf.begin() + *len);
        }
        ViewFile vf;
        vf.file_path = dir + "/b" + dstd::to_string(f) + ".pfw.gz";
        vf.index_path = dir + "/b" + dstd::to_string(f) + ".idx";
        {
            dftu_utils_test::FileOut out(vf.file_path);
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        files.push_back(std::move(vf));
    }
    return View::from_files(std::move(files)).count_scans(std::move(counter));
}

dstd::Vector<dstd::String> sorted_rows(const dataframe::DataFrame& f) {
    dstd::Vector<dstd::String> rows;
    for (std::int64_t r = 0; r < f.num_rows(); ++r) {
        dstd::String row;
        for (std::size_t c = 0; c < f.columns.size(); ++c)
            row += f.names[c] + "=" +
                   dstd::String(dataframe::cell_to_string(f.columns[c], r)) +
                   ";";
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

}  // namespace

constexpr std::uint64_t TINY_BUDGET = 4096;

TEST_SUITE("View - batched row plans") {
    TEST_CASE("plans that promise scan order read on their own") {
        TestEnvironment env(10);
        auto counter = dstd::make_shared<ScanCounter>();
        View t = batch_viewer(env.dir(), counter);
        const dataframe::LazyFrame ordered =
            t.duql("select name, dur, ts | where dur > 100").lazy();
        CHECK_FALSE(absorbed_source(ordered)->batch_key());
        CHECK(absorbed_source(t.lazy())->batch_key());
    }

    TEST_CASE("a mixed group streams its streamable plans in one scan") {
        TestEnvironment env(10);
        auto counter = dstd::make_shared<ScanCounter>();
        View t = batch_viewer(env.dir(), counter);
        const dstd::Vector<dataframe::LazyFrame> plans = {
            t.lazy(),
            t.duql(R"(cat == "POSIX")").lazy(),
            t.duql("dur > 400").select({"ts", "dur", "name"}),
            t.duql("select name, dur, ts | where dur > 100").lazy(),
            t.duql("derive ms = dur / 1000 | where ms < 0.2").lazy(),
        };
        dstd::Vector<dataframe::DataFrame> alone;
        dstd::Vector<std::uint64_t> scanned_alone;
        for (const auto& p : plans) {
            counter->events_scanned = 0;
            alone.push_back(test_view_common::run(p.collect()));
            scanned_alone.push_back(counter->events_scanned.load());
        }
        counter->events_scanned = 0;
        dstd::Vector<dataframe::DataFrame> together = test_view_common::run(
            dftracer::utils::dataframe::collect_all(plans));
        REQUIRE(together.size() == plans.size());
        // The three streamable plans share one scan; each ordered plan reads
        // the traces on its own.
        CHECK(counter->events_scanned.load() ==
              scanned_alone[0] + scanned_alone[3] + scanned_alone[4]);
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK(together[i].num_rows() == alone[i].num_rows());
            CHECK(sorted_rows(together[i]) == sorted_rows(alone[i]));
        }
        // The ordered plans return their rows in scan order, as alone.
        for (std::size_t i = 3; i < plans.size(); ++i)
            check_same_rows(together[i], alone[i]);
    }

    TEST_CASE("a session returns the same rows as collecting each plan") {
        TestEnvironment env(10);
        auto counter = dstd::make_shared<ScanCounter>();
        View t = batch_viewer(env.dir(), counter);
        const dstd::Vector<dataframe::LazyFrame> plans = {
            t.lazy(),
            t.duql("dur < 100").select({"ts", "dur", "name", "pid"}),
            t.duql("select name, dur, ts | where dur > 100").lazy(),
        };
        TraceSession s = t.session();
        dstd::Vector<Deferred<dataframe::DataFrame>> handles;
        for (const auto& p : plans) handles.push_back(s.collect(p));
        test_view_common::run(s.execute());
        for (std::size_t i = 0; i < plans.size(); ++i) {
            const dataframe::DataFrame want =
                test_view_common::run(plans[i].collect());
            if (i == 2)
                check_same_rows(handles[i].get(), want);
            else
                CHECK(sorted_rows(handles[i].get()) == sorted_rows(want));
        }
    }

    TEST_CASE("plans whose rows cannot stream read buffered and free it") {
        TestEnvironment env(10);
        auto counter = dstd::make_shared<ScanCounter>();
        View t = batch_viewer(env.dir(), counter);
        const dstd::Vector<dataframe::LazyFrame> plans = {
            t.phase(Phase::Metadata).lazy(),
            t.phase(Phase::Metadata).duql(R"(name == "process_name")").lazy(),
            t.phase(Phase::Metadata).duql(R"(name == "absent")").lazy(),
        };
        for (const auto& p : plans)
            CHECK_FALSE(absorbed_source(p)->streams_in_batch());
        dstd::Vector<dataframe::DataFrame> together = test_view_common::run(
            dftracer::utils::dataframe::collect_all(plans));
        CHECK(together[0].num_rows() == 1);
        for (std::size_t i = 0; i < plans.size(); ++i)
            CHECK(sorted_rows(together[i]) ==
                  sorted_rows(test_view_common::run(plans[i].collect())));
    }

    TEST_CASE("a member that stops early does not stall the streamed group") {
        TestEnvironment env(10);
        auto counter = dstd::make_shared<ScanCounter>();
        // A budget far below the rows of one plan: a read that waited on a
        // cursor nobody drains would stall.
        View t = batch_viewer(env.dir(), counter);
        const dstd::Vector<dataframe::LazyFrame> plans = {
            t.lazy().head(1).memory_budget(TINY_BUDGET),
            t.lazy().memory_budget(TINY_BUDGET),
            t.duql(R"(cat == "POSIX")")
                .lazy()
                .head(0)
                .memory_budget(TINY_BUDGET),
            t.duql("dur > 400").lazy().memory_budget(TINY_BUDGET),
        };
        dstd::Vector<dataframe::DataFrame> together = test_view_common::run(
            dftracer::utils::dataframe::collect_all(plans));
        CHECK(together[0].num_rows() == 1);
        CHECK(together[2].num_rows() == 0);
        CHECK(sorted_rows(together[1]) ==
              sorted_rows(test_view_common::run(plans[1].collect())));
        CHECK(sorted_rows(together[3]) ==
              sorted_rows(test_view_common::run(plans[3].collect())));
    }

    TEST_CASE("a session member that stops early does not stall the others") {
        TestEnvironment env(10);
        auto counter = dstd::make_shared<ScanCounter>();
        View t = batch_viewer(env.dir(), counter);
        const dstd::Vector<dataframe::LazyFrame> plans = {
            t.lazy().head(1).memory_budget(TINY_BUDGET),
            t.lazy().memory_budget(TINY_BUDGET),
            t.duql(R"(cat == "POSIX")")
                .lazy()
                .head(0)
                .memory_budget(TINY_BUDGET),
            t.duql("dur > 400").lazy().memory_budget(TINY_BUDGET),
        };
        TraceSession s = t.session();
        dstd::Vector<Deferred<dataframe::DataFrame>> handles;
        for (const auto& p : plans) handles.push_back(s.collect(p));
        test_view_common::run(s.execute());
        CHECK(handles[0].get().num_rows() == 1);
        CHECK(handles[2].get().num_rows() == 0);
        CHECK(sorted_rows(handles[1].get()) ==
              sorted_rows(test_view_common::run(plans[1].collect())));
        CHECK(sorted_rows(handles[3].get()) ==
              sorted_rows(test_view_common::run(plans[3].collect())));
    }

    TEST_CASE("a plan with a combine batches beside streaming plans") {
        TestEnvironment env(10);
        auto counter = dstd::make_shared<ScanCounter>();
        View t = batch_viewer(env.dir(), counter);
        const dstd::Vector<dataframe::LazyFrame> plans = {
            t.lazy(),
            t.duql(R"(cat == "STDIO")").lazy(),
            t.duql(R"(cat == "POSIX")")
                .lazy()
                .concat(t.duql("dur > 450").lazy()),
        };
        dstd::Vector<dataframe::DataFrame> together = test_view_common::run(
            dftracer::utils::dataframe::collect_all(plans));
        for (std::size_t i = 0; i < plans.size(); ++i)
            CHECK(sorted_rows(together[i]) ==
                  sorted_rows(test_view_common::run(plans[i].collect())));
    }
}
