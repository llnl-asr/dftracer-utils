// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/frame_ops.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/op.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

Series i64s(const dstd::Vector<std::int64_t>& v) {
    return Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size()));
}

using I = dstd::Optional<std::int64_t>;
const I NI = dstd::nullopt;

dstd::Vector<I> i64_col(const DataFrame& df, const dstd::String& name) {
    Series c = df.column(name);
    REQUIRE(c.valid());
    c = c.materialize();
    dstd::Vector<I> out;
    for (std::int64_t i = 0; i < c.length(); ++i) {
        if (c.is_null(i))
            out.emplace_back(dstd::nullopt);
        else
            out.emplace_back(c.data<std::int64_t>()[i]);
    }
    return out;
}

// pid = [1, 1, 1, 2], ts = [10, 20, 30, 5], dur = [1, 2, 3, 4]
DataFrame make_events() {
    DataFrame df;
    df.names = {"pid", "ts", "dur"};
    df.columns.push_back(i64s({1, 1, 1, 2}));
    df.columns.push_back(i64s({10, 20, 30, 5}));
    df.columns.push_back(i64s({1, 2, 3, 4}));
    return df;
}

dftu_dataframe* to_abi(const DataFrame& df) {
    dstd::Vector<const char*> names;
    dstd::Vector<dftu_series*> cols;
    for (std::size_t i = 0; i < df.names.size(); ++i) {
        names.push_back(df.names[i].c_str());
        cols.push_back(df.columns[i].share().release());
    }
    return dftu_dataframe_new(names.data(), cols.data(),
                              static_cast<int32_t>(names.size()));
}

}  // namespace

TEST_SUITE("frame ops") {
    TEST_CASE("window: row_number, running_sum and lag by name") {
        dstd::Vector<dataframe::WindowColumn> specs(3);
        specs[0].func = dataframe::WindowFunc::RowNumber;
        specs[0].out = "rn";
        specs[1].func = dataframe::WindowFunc::RunningSum;
        specs[1].set_value("dur");
        specs[1].out = "cum";
        specs[2].func = dataframe::WindowFunc::Lag;
        specs[2].set_value("dur");
        specs[2].params.offset = 1;
        specs[2].out = "prev";
        DataFrame out =
            dataframe::window(make_events(), {"pid"}, {"ts"}, specs);
        CHECK(out.names == dstd::Vector<dstd::String>{"pid", "ts", "dur", "rn",
                                                      "cum", "prev"});
        CHECK(i64_col(out, "rn") == dstd::Vector<I>{1, 2, 3, 1});
        CHECK(i64_col(out, "cum") == dstd::Vector<I>{1, 3, 6, 4});
        CHECK(i64_col(out, "prev") == dstd::Vector<I>{NI, 1, 2, NI});
        specs[1].set_value("nope");
        CHECK_THROWS_AS(
            dataframe::window(make_events(), {"pid"}, {"ts"}, specs),
            std::out_of_range);
    }

    TEST_CASE("gap_fill: locf over a grid, with and without a range") {
        DataFrame df;
        df.names = {"pid", "ts", "v"};
        df.columns.push_back(i64s({1, 1, 1}));
        df.columns.push_back(i64s({0, 10, 30}));
        df.columns.push_back(i64s({100, 110, 130}));
        DataFrame locf =
            dataframe::gap_fill(df, {"pid"}, "ts", 10, {"v"},
                                dataframe::GapFillMode::Locf, dstd::nullopt);
        CHECK(i64_col(locf, "ts") == dstd::Vector<I>{0, 10, 20, 30});
        CHECK(i64_col(locf, "v") == dstd::Vector<I>{100, 110, 110, 130});
        DataFrame ranged = dataframe::gap_fill(
            df, {"pid"}, "ts", 10, {"v"}, dataframe::GapFillMode::None,
            dstd::Pair<std::int64_t, std::int64_t>{0, 40});
        CHECK(i64_col(ranged, "ts") == dstd::Vector<I>{0, 10, 20, 30, 40});
        CHECK(i64_col(ranged, "v") == dstd::Vector<I>{100, 110, NI, 130, NI});
    }

    TEST_CASE(
        "asof: backward match within a partition, tolerance drops far "
        "rows") {
        DataFrame left;
        left.names = {"pid", "ts", "x"};
        left.columns.push_back(i64s({1, 1, 2}));
        left.columns.push_back(i64s({10, 25, 10}));
        left.columns.push_back(i64s({1, 2, 3}));
        DataFrame right;
        right.names = {"pid", "ts", "y"};
        right.columns.push_back(i64s({1, 1, 2}));
        right.columns.push_back(i64s({5, 20, 50}));
        right.columns.push_back(i64s({50, 200, 500}));
        DataFrame out =
            dataframe::asof(left, right, "ts", {"pid"},
                            dataframe::AsofDirection::Backward, dstd::nullopt);
        CHECK(out.names == dstd::Vector<dstd::String>{"pid", "ts", "x", "y"});
        CHECK(i64_col(out, "x") == dstd::Vector<I>{1, 2, 3});
        CHECK(i64_col(out, "y") == dstd::Vector<I>{50, 200, NI});
        DataFrame tight = dataframe::asof(
            left, right, "ts", {"pid"}, dataframe::AsofDirection::Backward, 3);
        CHECK(i64_col(tight, "y") == dstd::Vector<I>{NI, NI, NI});
        DataFrame fwd =
            dataframe::asof(left, right, "ts", {"pid"},
                            dataframe::AsofDirection::Forward, dstd::nullopt);
        CHECK(i64_col(fwd, "y") == dstd::Vector<I>{200, NI, 500});
    }

    TEST_CASE("interval: point in [lo, hi], outer keeps the unmatched point") {
        DataFrame left;
        left.names = {"p"};
        left.columns.push_back(i64s({5, 15, 99}));
        DataFrame right;
        right.names = {"lo", "hi", "tag"};
        right.columns.push_back(i64s({0, 10}));
        right.columns.push_back(i64s({10, 20}));
        right.columns.push_back(i64s({1, 2}));
        DataFrame inner =
            dataframe::interval(left, right, "p", "lo", "hi", {}, false);
        CHECK(inner.names == dstd::Vector<dstd::String>{"p", "tag"});
        CHECK(i64_col(inner, "p") == dstd::Vector<I>{5, 15});
        CHECK(i64_col(inner, "tag") == dstd::Vector<I>{1, 2});
        DataFrame outer =
            dataframe::interval(left, right, "p", "lo", "hi", {}, true);
        CHECK(i64_col(outer, "p") == dstd::Vector<I>{5, 15, 99});
        CHECK(i64_col(outer, "tag") == dstd::Vector<I>{1, 2, NI});
    }

    TEST_CASE("the four ops as lazy plan steps through frame_op") {
        auto scan = [](DataFrame df) {
            return dataframe::LazyFrame::scan(
                dstd::make_shared<dataframe::InMemorySource>(std::move(df)));
        };
        auto run = [](dftracer::utils::coro::CoroTask<DataFrame> t) {
            return dftracer::utils::default_runtime()
                .submit(std::move(t))
                .get();
        };
        const char* pid[1] = {"pid"};
        const char* ts[1] = {"ts"};
        dftu_window_spec specs[2] = {};
        specs[0].func = DFTU_WINDOW_ROW_NUMBER;
        specs[0].out = "rn";
        specs[1].func = DFTU_WINDOW_RUNNING_SUM;
        specs[1].value = "dur";
        specs[1].out = "cum";
        OpArgs warg;
        warg.strlist(1, pid, 1).strlist(2, ts, 1).winlist(3, specs);
        dataframe::LazyFrame plan =
            scan(make_events()).frame_op("dftu.frame.window", warg);
        CHECK(plan.schema().empty());
        DataFrame w = run(plan.collect(1));
        CHECK(w.names ==
              dstd::Vector<dstd::String>{"pid", "ts", "dur", "rn", "cum"});
        CHECK(i64_col(w, "cum") == dstd::Vector<I>{1, 3, 6, 4});

        OpArgs aarg;
        aarg.str(2, "ts")
            .strlist(3, pid, 1)
            .i32(4, DFTU_ASOF_BACKWARD)
            .f64(5, -1);
        DataFrame a =
            run(scan(make_events())
                    .frame_op("dftu.frame.asof", aarg, {scan(make_events())})
                    .collect(1));
        CHECK(i64_col(a, "dur_right") == dstd::Vector<I>{1, 2, 3, 4});
    }

    TEST_CASE("C ABI and registry: the four ops are registered and run") {
        const dftu_op_desc* window_op = dftu_op_find("dftu.frame.window");
        const dftu_op_desc* gap_op = dftu_op_find("dftu.frame.gap_fill");
        const dftu_op_desc* asof_op = dftu_op_find("dftu.frame.asof");
        const dftu_op_desc* interval_op = dftu_op_find("dftu.frame.interval");
        REQUIRE(window_op);
        REQUIRE(gap_op);
        REQUIRE(asof_op);
        REQUIRE(interval_op);
        CHECK(dftu_op_arity(window_op->sig) == 1);
        CHECK(dftu_op_arity(asof_op->sig) == 2);

        dftu_dataframe* ev = to_abi(make_events());
        REQUIRE(ev);
        const dftu_dataframe* fin[1] = {ev};
        const char* pid[1] = {"pid"};
        const char* ts[1] = {"ts"};
        dftu_window_spec specs[2] = {};
        specs[0].func = DFTU_WINDOW_ROW_NUMBER;
        specs[0].out = "rn";
        specs[1].func = DFTU_WINDOW_RUNNING_SUM;
        specs[1].value = "dur";
        specs[1].out = "cum";
        OpArgs warg;
        warg.strlist(1, pid, 1).strlist(2, ts, 1).winlist(3, specs);
        dftu_dataframe* w = dftu_op_run_frame(window_op, fin, 1, warg);
        REQUIRE(w);
        CHECK(dftu_dataframe_num_columns(w) == 5);
        CHECK(dstd::String(dftu_dataframe_column_name(w, 4)) == "cum");
        CHECK(dftu_dataframe_num_rows(w) == 4);
        dftu_dataframe* direct =
            dftu_dataframe_window(ev, pid, 1, ts, 1, specs, 2);
        REQUIRE(direct);
        CHECK(dftu_dataframe_num_columns(direct) == 5);
        static_assert(sizeof(dftu_window_func) == sizeof(int));
        const int bad = 32;
        std::memcpy(&specs[1].func, &bad, sizeof(bad));
        CHECK(dftu_dataframe_window(ev, pid, 1, ts, 1, specs, 2) == nullptr);
        dftu_dataframe_free(direct);
        dftu_dataframe_free(w);

        const char* v[1] = {"dur"};
        const std::int64_t range[2] = {0, 40};
        OpArgs garg;
        garg.strlist(1, pid, 1)
            .str(2, "ts")
            .i64(3, 10)
            .strlist(4, v, 1)
            .i32(5, DFTU_GAP_FILL_NONE)
            .i64list(6, range);
        dftu_dataframe* g = dftu_op_run_frame(gap_op, fin, 1, garg);
        REQUIRE(g);
        // pid 1 spans 0..40 (5 points), pid 2 spans 0..40 (5 points).
        CHECK(dftu_dataframe_num_rows(g) == 10);
        dftu_dataframe_free(g);
        CHECK(dftu_dataframe_gap_fill(ev, pid, 1, "ts", 10, v, 1,
                                      static_cast<dftu_gap_fill_mode>(3),
                                      nullptr, 0) == nullptr);
        CHECK(dftu_dataframe_gap_fill(ev, pid, 1, "ts", 10, v, 1,
                                      DFTU_GAP_FILL_NONE, range, 1) == nullptr);

        const dftu_dataframe* pair[2] = {ev, ev};
        OpArgs aarg;
        aarg.str(2, "ts")
            .strlist(3, pid, 1)
            .i32(4, DFTU_ASOF_BACKWARD)
            .f64(5, -1);
        dftu_dataframe* a = dftu_op_run_frame(asof_op, pair, 2, aarg);
        REQUIRE(a);
        CHECK(dftu_dataframe_num_rows(a) == 4);
        CHECK(dstd::String(dftu_dataframe_column_name(a, 3)) == "dur_right");
        dftu_dataframe_free(a);
        CHECK(dftu_dataframe_asof(ev, ev, "ts", pid, 1,
                                  static_cast<dftu_asof_direction>(3),
                                  -1) == nullptr);

        OpArgs iarg;
        iarg.str(2, "ts").str(3, "ts").str(4, "dur").strlist(5, pid, 1).i32(6,
                                                                            0);
        dftu_dataframe* iv = dftu_op_run_frame(interval_op, pair, 2, iarg);
        REQUIRE(iv);
        // A point matches its own [ts, dur] span only where ts <= dur: never.
        CHECK(dftu_dataframe_num_rows(iv) == 0);
        dftu_dataframe_free(iv);
        CHECK(dftu_dataframe_interval(ev, ev, "nope", "ts", "dur", pid, 1, 0) ==
              nullptr);
        dftu_dataframe_free(ev);
    }
}
