// SPDX-License-Identifier: MIT
// Registered UDAFs as window functions: a UDAF with retract and value slides
// its frame, any other is recomputed over each frame, and both agree with the
// built-in FRAME_SUM over the same frame, partitions, nulls and min_count
// included; the errors a UDAF or its call reports reach the caller.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/frame_ops.h>
#include <dftracer/utils/dataframe/internal/dataframe_handle.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/op.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <cstdint>

using namespace dftracer::utils;
using dataframe::DataFrame;
using dataframe::Series;
using dataframe::WindowColumn;
using dataframe::WindowFrameMode;
using dataframe::WindowFunc;

#ifndef UDAF_PLUGIN_PATH
#error "UDAF_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

constexpr std::int64_t ROWS = 90;
constexpr std::int64_t UNBOUNDED = dataframe::WINDOW_UNBOUNDED;

using Cells = dstd::Vector<dstd::Optional<std::int64_t>>;

dstd::Optional<plugins::host::Plugins> load() {
    auto built =
        plugins::host::Plugins::builder().add(UDAF_PLUGIN_PATH).build();
    INFO((built.has_value() ? dstd::String{}
                            : dstd::String(built.error().message())));
    REQUIRE(built.has_value());
    return std::move(*built);
}

bool has(dstd::StringView text, dstd::StringView word) {
    return text.find(word) != dstd::StringView::npos;
}

// Partition p = i % 3, order o = i, value v = i % 13; every fifth v is null.
DataFrame make_frame() {
    dstd::Vector<std::int64_t> p, o, v;
    dstd::Vector<std::uint8_t> valid((ROWS + 7) / 8, 0);
    for (std::int64_t i = 0; i < ROWS; ++i) {
        p.push_back(i % 3);
        o.push_back(i);
        v.push_back(i % 13);
        if ((i + 1) % 5 != 0)
            valid[static_cast<std::size_t>(i >> 3)] |=
                static_cast<std::uint8_t>(1u << (i & 7));
    }
    DataFrame df;
    df.names.push_back(dstd::SharedString{"p"});
    df.names.push_back(dstd::SharedString{"o"});
    df.names.push_back(dstd::SharedString{"v"});
    df.columns.push_back(Series::flat_i64(p.data(), ROWS, nullptr));
    df.columns.push_back(Series::flat_i64(o.data(), ROWS, nullptr));
    df.columns.push_back(Series::flat_i64(v.data(), ROWS, valid.data()));
    return df;
}

WindowColumn frame_of(WindowFunc func, const char* out, std::int64_t min_count,
                      std::int64_t preceding, std::int64_t following,
                      WindowFrameMode mode) {
    WindowColumn w;
    w.func = func;
    w.out = out;
    w.params.frame = {min_count, preceding, following, mode, 0.0};
    return w;
}

WindowColumn udaf_of(const char* name, const char* out, std::int64_t min_count,
                     std::int64_t preceding, std::int64_t following,
                     WindowFrameMode mode) {
    WindowColumn w = frame_of(WindowFunc::FrameUdaf, out, min_count, preceding,
                              following, mode);
    w.set_udaf(name, {"v"});
    return w;
}

WindowColumn sum_of(const char* out, std::int64_t min_count,
                    std::int64_t preceding, std::int64_t following,
                    WindowFrameMode mode) {
    WindowColumn w = frame_of(WindowFunc::FrameSum, out, min_count, preceding,
                              following, mode);
    w.set_value("v");
    return w;
}

Cells cells(const DataFrame& f, dstd::StringView name) {
    std::size_t c = 0;
    while (c < f.names.size() && f.names[c] != name) ++c;
    REQUIRE(c < f.names.size());
    const Series col = f.columns[c].materialize();
    REQUIRE(col.type() == dataframe::TypeId::Int64);
    Cells out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r)
        out.push_back(col.is_null(r) ? dstd::Optional<std::int64_t>{}
                                     : dstd::Optional<std::int64_t>{
                                           col.data<std::int64_t>()[r]});
    return out;
}

DataFrame run(const dstd::Vector<WindowColumn>& specs) {
    auto got = dataframe::window(make_frame(), {"p"}, {"o"}, specs);
    INFO(
        (got.has_value() ? dstd::String{} : dstd::String(got.error().c_str())));
    REQUIRE(got.has_value());
    return std::move(*got);
}

dstd::String failure(const dstd::Vector<WindowColumn>& specs) {
    auto got = dataframe::window(make_frame(), {"p"}, {"o"}, specs);
    return got.has_value() ? dstd::String{} : dstd::String(got.error().c_str());
}

struct Frame {
    const char* label;
    std::int64_t preceding;
    std::int64_t following;
    WindowFrameMode mode;
};

constexpr Frame FRAMES[] = {
    {"rows 3 back", 3, 0, WindowFrameMode::Rows},
    {"rows 2 back 1 ahead", 2, 1, WindowFrameMode::Rows},
    {"rows running", UNBOUNDED, 0, WindowFrameMode::Rows},
    {"rows whole", UNBOUNDED, UNBOUNDED, WindowFrameMode::Rows},
    {"rows 5 ahead", 0, 5, WindowFrameMode::Rows},
    {"range 8 back", 8, 0, WindowFrameMode::Range},
    {"range 6 back 3 ahead", 6, 3, WindowFrameMode::Range},
};

}  // namespace

TEST_CASE("the sliding and the recomputing UDAF equal the built-in sum") {
    auto set = load();
    for (const Frame& f : FRAMES) {
        for (const std::int64_t min_count : {0, 1, 2}) {
            INFO(f.label);
            INFO(min_count);
            const DataFrame out =
                run({sum_of("b", min_count, f.preceding, f.following, f.mode),
                     udaf_of("usumr", "slide", min_count, f.preceding,
                             f.following, f.mode),
                     udaf_of("usum", "recompute", min_count, f.preceding,
                             f.following, f.mode)});
            const Cells want = cells(out, "b");
            const Cells slide = cells(out, "slide");
            const Cells recompute = cells(out, "recompute");
            REQUIRE(slide.size() == want.size());
            for (std::size_t r = 0; r < want.size(); ++r) {
                // The built-in sum of an empty frame is null; with min_count 0
                // a UDAF frame holds no present value, so it is null too.
                CHECK(slide[r] == want[r]);
                CHECK(recompute[r] == want[r]);
            }
        }
    }
}

TEST_CASE("a window frame sum of known values") {
    auto set = load();
    // Partition 0 is the rows i = 0, 3, 6, 9, 12 with v = 0, 3, 6, null, 12;
    // each frame is the row and the two before it.
    const DataFrame out =
        run({udaf_of("usumr", "s", 1, 2, 0, WindowFrameMode::Rows),
             udaf_of("usum", "t", 1, 2, 0, WindowFrameMode::Rows)});
    const Cells s = cells(out, "s");
    CHECK(s[0] == dstd::Optional<std::int64_t>{0});
    CHECK(s[1] == dstd::Optional<std::int64_t>{3});
    CHECK(s[2] == dstd::Optional<std::int64_t>{9});
    CHECK(s[3] == dstd::Optional<std::int64_t>{9});
    CHECK(s[4] == dstd::Optional<std::int64_t>{18});
    CHECK(cells(out, "t") == s);
}

TEST_CASE("the errors of a UDAF window reach the caller") {
    auto set = load();
    const dstd::String unknown =
        failure({udaf_of("nosuchagg", "x", 0, 1, 0, WindowFrameMode::Rows)});
    CHECK(has(unknown, "nosuchagg"));

    WindowColumn two = udaf_of("usum", "x", 0, 1, 0, WindowFrameMode::Rows);
    two.set_udaf("usum", {"v", "o"});
    const dstd::String arity = failure({two});
    CHECK(has(arity, "usum"));

    WindowColumn missing = udaf_of("usum", "x", 0, 1, 0, WindowFrameMode::Rows);
    missing.set_udaf("usum", {"nosuchcolumn"});
    CHECK(has(failure({missing}), "nosuchcolumn"));

    const dstd::String update =
        failure({udaf_of("ufail", "x", 0, 1, 0, WindowFrameMode::Rows)});
    CHECK(has(update, "refused on purpose"));
    const dstd::String retract =
        failure({udaf_of("uretfail", "x", 0, 1, 0, WindowFrameMode::Rows)});
    CHECK(has(retract, "retract refused"));

    const dstd::String negative =
        failure({udaf_of("usumr", "x", 0, -2, 0, WindowFrameMode::Rows)});
    CHECK_FALSE(negative.empty());
}

TEST_CASE("a UDAF value that does not fit its result type is an error") {
    auto set = load();
    const dstd::String neg =
        failure({udaf_of("uwneg", "x", 0, 1, 0, WindowFrameMode::Rows)});
    CHECK(has(neg, "uwneg"));
    CHECK(has(neg, "does not fit"));
    const dstd::String big =
        failure({udaf_of("uwbig", "x", 0, 1, 0, WindowFrameMode::Rows)});
    CHECK(has(big, "uwbig"));
    CHECK(has(big, "does not fit"));
}

TEST_CASE("the window C ABI refuses a malformed UDAF spec") {
    auto set = load();
    dftu_dataframe* h = dataframe::dataframe_handle_wrap(make_frame());
    const char* part[] = {"p"};
    const char* order[] = {"o"};
    const char* names[] = {"v"};
    const char* null_names[] = {nullptr};

    dftu_window_spec s{};
    s.func = DFTU_WINDOW_FRAME_UDAF;
    s.out = "x";
    s.param.udaf = {
        0,       1,
        0,       static_cast<dftu_window_frame_mode>(DFTU_WINDOW_FRAME_ROWS),
        "usumr", names,
        1};
    const auto window = [&](const dftu_window_spec& spec) {
        return dftu_dataframe_window(h, part, 1, order, 1, &spec, 1);
    };

    dftu_dataframe* ok = window(s);
    CHECK(ok != nullptr);
    dftu_dataframe_free(ok);

    dftu_window_spec bad = s;
    bad.param.udaf.args = nullptr;
    CHECK(window(bad) == nullptr);
    bad = s;
    bad.param.udaf.args = null_names;
    CHECK(window(bad) == nullptr);
    bad = s;
    bad.param.udaf.name = nullptr;
    CHECK(window(bad) == nullptr);
    bad = s;
    bad.param.udaf.arg_count = -1;
    CHECK(window(bad) == nullptr);
    bad = s;
    bad.param.udaf.arg_count = 0;
    bad.param.udaf.args = nullptr;
    CHECK(window(bad) == nullptr);
    dftu_dataframe_free(h);
}

TEST_CASE(
    "a lazy window with a UDAF equals the in-memory window under a "
    "budget") {
    auto set = load();
    const DataFrame df = make_frame();
    const dstd::Vector<WindowColumn> specs = {
        sum_of("b", 1, 3, 0, WindowFrameMode::Rows),
        udaf_of("usumr", "slide", 1, 3, 0, WindowFrameMode::Rows),
        udaf_of("usum", "recompute", 1, 6, 0, WindowFrameMode::Range)};
    const DataFrame want = run(specs);

    dstd::Vector<dftu_window_spec> raw;
    dstd::Vector<dstd::String> keep = dstd::to_strings(df.names);
    dstd::Vector<dstd::Vector<const char*>> arg_names;
    arg_names.reserve(specs.size());
    for (const WindowColumn& w : specs) {
        dftu_window_spec s{};
        s.func = static_cast<dftu_window_func>(w.func);
        s.out = w.out.c_str();
        if (w.func == WindowFunc::FrameUdaf) {
            arg_names.push_back({"v"});
            s.param.udaf = {
                w.params.frame.min_count,
                w.params.frame.preceding,
                w.params.frame.following,
                static_cast<dftu_window_frame_mode>(w.params.frame.mode),
                w.udaf()->c_str(),
                arg_names.back().data(),
                1};
        } else {
            s.value = "v";
            s.param.frame = {
                w.params.frame.min_count,
                w.params.frame.preceding,
                w.params.frame.following,
                static_cast<dftu_window_frame_mode>(w.params.frame.mode),
                0.0,
                nullptr};
        }
        raw.push_back(s);
        keep.push_back(w.out);
    }
    const char* part[] = {"p"};
    const char* order[] = {"o"};
    dataframe::OpArgs args;
    args.strlist(1, part, 1).strlist(2, order, 1).winlist(3, raw);
    const dataframe::LazyFrame plan =
        df.lazy().frame_op("dftu.frame.window", args, {}, keep);

    const auto collected = [](dataframe::LazyFrame p) {
        DataFrame out =
            dftu_utils_test::ok(default_runtime().submit(p.collect(16)).get());
        for (Series& c : out.columns) c = c.materialize();
        return out;
    };
    for (const DataFrame& got :
         {collected(plan), collected(plan.memory_budget(4096))}) {
        for (const char* name : {"b", "slide", "recompute"})
            CHECK(cells(got, name) == cells(want, name));
    }
}
