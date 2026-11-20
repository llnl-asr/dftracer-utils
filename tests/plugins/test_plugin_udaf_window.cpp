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
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dlfcn.h>
#include <doctest/doctest.h>
#include <span_list.h>
#include <task_result.h>

#include <algorithm>
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

WindowColumn frame_of(WindowFunc func, dstd::StringView out,
                      std::int64_t min_count, std::int64_t preceding,
                      std::int64_t following, WindowFrameMode mode) {
    WindowColumn w;
    w.func = func;
    w.out = out;
    w.params.frame = {min_count, preceding, following, mode, 0.0};
    return w;
}

WindowColumn udaf_of(dstd::String name, dstd::StringView out,
                     std::int64_t min_count, std::int64_t preceding,
                     std::int64_t following, WindowFrameMode mode) {
    WindowColumn w = frame_of(WindowFunc::FrameUdaf, out, min_count, preceding,
                              following, mode);
    w.set_udaf(name, {"v"});
    return w;
}

WindowColumn sum_of(dstd::StringView out, std::int64_t min_count,
                    std::int64_t preceding, std::int64_t following,
                    WindowFrameMode mode) {
    WindowColumn w = frame_of(WindowFunc::FrameSum, out, min_count, preceding,
                              following, mode);
    w.set_value("v");
    return w;
}

Cells cells(const DataFrame& f, dstd::String name) {
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

DataFrame run(dstd::Vector<WindowColumn> specs) {
    auto got = dataframe::window(
        make_frame(), dftu_utils_test::span_of<dstd::String>({"p"}),
        dftu_utils_test::span_of<dstd::String>({"o"}), specs);
    INFO(
        (got.has_value() ? dstd::String{} : dstd::String(got.error().c_str())));
    REQUIRE(got.has_value());
    return std::move(*got);
}

dstd::String failure(dstd::Vector<WindowColumn> specs) {
    auto got = dataframe::window(
        make_frame(), dftu_utils_test::span_of<dstd::String>({"p"}),
        dftu_utils_test::span_of<dstd::String>({"o"}), specs);
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
    args.strlist(1, {part, 1}).strlist(2, {order, 1}).winlist(3, raw);
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

namespace {

// Partition p = i % parts, order o = i, value v = (i * 37) % 101 - 50; every
// fifth v is null.
DataFrame make_big(std::int64_t rows, std::int64_t parts) {
    dstd::Vector<std::int64_t> p, o, v;
    dstd::Vector<std::uint8_t> valid(static_cast<std::size_t>((rows + 7) / 8),
                                     0);
    for (std::int64_t i = 0; i < rows; ++i) {
        p.push_back(i % parts);
        o.push_back(i);
        v.push_back((i * 37) % 101 - 50);
        if ((i + 1) % 5 != 0)
            valid[static_cast<std::size_t>(i >> 3)] |=
                static_cast<std::uint8_t>(1u << (i & 7));
    }
    DataFrame df;
    df.names.push_back(dstd::SharedString{"p"});
    df.names.push_back(dstd::SharedString{"o"});
    df.names.push_back(dstd::SharedString{"v"});
    df.columns.push_back(Series::flat_i64(p.data(), rows, nullptr));
    df.columns.push_back(Series::flat_i64(o.data(), rows, nullptr));
    df.columns.push_back(Series::flat_i64(v.data(), rows, valid.data()));
    return df;
}

DataFrame run_on(const DataFrame& df, dstd::Vector<WindowColumn> specs) {
    auto got =
        dataframe::window(df, dftu_utils_test::span_of<dstd::String>({"p"}),
                          dftu_utils_test::span_of<dstd::String>({"o"}), specs);
    INFO(
        (got.has_value() ? dstd::String{} : dstd::String(got.error().c_str())));
    REQUIRE(got.has_value());
    return std::move(*got);
}

using DoubleCells = dstd::Vector<dstd::Optional<double>>;

DoubleCells double_cells(const DataFrame& f, dstd::String name) {
    std::size_t c = 0;
    while (c < f.names.size() && f.names[c] != name) ++c;
    REQUIRE(c < f.names.size());
    const Series col = f.columns[c].materialize();
    REQUIRE(col.type() == dataframe::TypeId::Float64);
    DoubleCells out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r)
        out.push_back(col.is_null(r)
                          ? dstd::Optional<double>{}
                          : dstd::Optional<double>{col.data<double>()[r]});
    return out;
}

// What p50 gives over a rows frame, from the definition: the element at
// count / 2 of the sorted present values of the frame. The window gives its
// rows in (partition, order) order, which is the order here.
DoubleCells p50_expected(const DataFrame& df, std::int64_t parts,
                         std::int64_t preceding, std::int64_t following,
                         std::int64_t min_count) {
    const std::int64_t rows = df.num_rows();
    const Series v = df.columns[2].materialize();
    DoubleCells out;
    for (std::int64_t part = 0; part < parts; ++part) {
        const std::int64_t size = (rows - part + parts - 1) / parts;
        for (std::int64_t j = 0; j < size; ++j) {
            const std::int64_t lo =
                preceding == UNBOUNDED
                    ? 0
                    : std::max<std::int64_t>(0, j - preceding);
            const std::int64_t hi = following == UNBOUNDED
                                        ? size - 1
                                        : std::min(size - 1, j + following);
            dstd::Vector<double> values;
            for (std::int64_t k = lo; k <= hi; ++k) {
                const std::int64_t row = k * parts + part;
                if (!v.is_null(row))
                    values.push_back(
                        static_cast<double>(v.data<std::int64_t>()[row]));
            }
            if (values.empty() ||
                static_cast<std::int64_t>(values.size()) < min_count) {
                out.push_back({});
                continue;
            }
            std::nth_element(values.begin(), values.begin() + values.size() / 2,
                             values.end());
            out.push_back(values[values.size() / 2]);
        }
    }
    return out;
}

constexpr Frame BIG_FRAMES[] = {
    {"rows 1", 0, 0, WindowFrameMode::Rows},
    {"rows 2", 1, 0, WindowFrameMode::Rows},
    {"rows 3", 2, 0, WindowFrameMode::Rows},
    {"rows 7", 6, 0, WindowFrameMode::Rows},
    {"rows 64", 63, 0, WindowFrameMode::Rows},
    {"rows 1000", 999, 0, WindowFrameMode::Rows},
    {"rows 3 around", 1, 1, WindowFrameMode::Rows},
    {"rows 20 ahead", 5, 14, WindowFrameMode::Rows},
    {"rows running", UNBOUNDED, 0, WindowFrameMode::Rows},
    {"rows to end", 0, UNBOUNDED, WindowFrameMode::Rows},
    {"rows whole", UNBOUNDED, UNBOUNDED, WindowFrameMode::Rows},
    {"range 1", 0, 0, WindowFrameMode::Range},
    {"range 2", 2, 0, WindowFrameMode::Range},
    {"range 3", 4, 0, WindowFrameMode::Range},
    {"range 7", 12, 0, WindowFrameMode::Range},
    {"range 64 around", 60, 60, WindowFrameMode::Range},
    {"range 1000", 1998, 0, WindowFrameMode::Range},
};

}  // namespace

TEST_CASE(
    "a sliding, a tree and a recomputing UDAF equal the built-in sum over "
    "many frames") {
    auto set = load();
    const DataFrame df = make_big(4200, 2);
    for (const Frame& f : BIG_FRAMES) {
        for (const std::int64_t min_count : {0, 1, 3}) {
            INFO(f.label);
            INFO(min_count);
            const DataFrame out = run_on(
                df, {sum_of("b", min_count, f.preceding, f.following, f.mode),
                     udaf_of("usumr", "slide", min_count, f.preceding,
                             f.following, f.mode),
                     udaf_of("usum", "plain", min_count, f.preceding,
                             f.following, f.mode),
                     udaf_of("userial", "serial", min_count, f.preceding,
                             f.following, f.mode)});
            const Cells want = cells(out, "b");
            for (const char* name : {"slide", "plain", "serial"}) {
                INFO(name);
                const Cells got = cells(out, name);
                REQUIRE(got.size() == want.size());
                std::size_t wrong = 0;
                for (std::size_t r = 0; r < want.size(); ++r)
                    if (got[r] != want[r]) ++wrong;
                CHECK(wrong == 0);
            }
        }
    }
}

TEST_CASE("the frame segments of the slide agree over partition counts") {
    auto set = load();
    for (const std::int64_t parts : {1, 3, 700}) {
        const DataFrame df = make_big(5000, parts);
        for (const std::int64_t back : {4, 300, 5000}) {
            INFO(parts);
            INFO(back);
            const DataFrame out = run_on(
                df,
                {sum_of("b", 1, back, 2, WindowFrameMode::Rows),
                 udaf_of("usumr", "slide", 1, back, 2, WindowFrameMode::Rows),
                 udaf_of("usum", "plain", 1, back, 2, WindowFrameMode::Rows)});
            CHECK(cells(out, "slide") == cells(out, "b"));
            CHECK(cells(out, "plain") == cells(out, "b"));
        }
    }
}

TEST_CASE("a tree UDAF of one partition larger than a chunk equals the sum") {
    auto set = load();
    const DataFrame df = make_big(300000, 1);
    const DataFrame out = run_on(
        df, {sum_of("b", 1, 99, 0, WindowFrameMode::Rows),
             udaf_of("usum", "plain", 1, 99, 0, WindowFrameMode::Rows),
             udaf_of("usumr", "slide", 1, 99, 0, WindowFrameMode::Rows)});
    CHECK(cells(out, "plain") == cells(out, "b"));
    CHECK(cells(out, "slide") == cells(out, "b"));
}

TEST_CASE("a window over no rows gives no rows") {
    auto set = load();
    const DataFrame df = make_big(0, 1);
    const DataFrame out = run_on(
        df, {udaf_of("usum", "plain", 0, 20, 0, WindowFrameMode::Rows),
             udaf_of("usumr", "slide", 0, 20, 0, WindowFrameMode::Rows)});
    CHECK(out.num_rows() == 0);
}

TEST_CASE("a combine failure of a tree window reaches the caller") {
    auto set = load();
    const DataFrame df = make_big(300, 2);
    auto got = dataframe::window(
        df, dftu_utils_test::span_of<dstd::String>({"p"}),
        dftu_utils_test::span_of<dstd::String>({"o"}),
        dftu_utils_test::span_of<dftracer::utils::dataframe::WindowColumn>(
            {udaf_of("ufailcombine", "x", 0, 20, 0, WindowFrameMode::Rows)}));
    REQUIRE_FALSE(got.has_value());
    CHECK(has(dstd::String(got.error().c_str()), "refused merge"));
}

namespace {

std::int64_t counter(dstd::CStringView symbol) {
    void* h = dlopen(UDAF_PLUGIN_PATH, RTLD_NOW | RTLD_NOLOAD);
    REQUIRE(h != nullptr);
    auto* fn = reinterpret_cast<std::int64_t (*)()>(dlsym(h, symbol.c_str()));
    REQUIRE(fn != nullptr);
    const std::int64_t value = fn();
    dlclose(h);
    return value;
}

// What uhash gives over a rows frame, from the definition: h = h * 31 + x over
// the present values of the frame in row order, in (partition, order) order.
Cells hash_expected(const DataFrame& df, std::int64_t parts,
                    std::int64_t preceding, std::int64_t following,
                    std::int64_t min_count) {
    const std::int64_t rows = df.num_rows();
    const Series v = df.columns[2].materialize();
    Cells out;
    for (std::int64_t part = 0; part < parts; ++part) {
        const std::int64_t size = (rows - part + parts - 1) / parts;
        for (std::int64_t j = 0; j < size; ++j) {
            const std::int64_t lo =
                preceding == UNBOUNDED
                    ? 0
                    : std::max<std::int64_t>(0, j - preceding);
            const std::int64_t hi = following == UNBOUNDED
                                        ? size - 1
                                        : std::min(size - 1, j + following);
            std::uint64_t h = 0;
            std::int64_t count = 0;
            for (std::int64_t k = lo; k <= hi; ++k) {
                const std::int64_t row = k * parts + part;
                if (v.is_null(row)) continue;
                h = h * 31u +
                    static_cast<std::uint64_t>(v.data<std::int64_t>()[row]);
                ++count;
            }
            if (count == 0 || count < min_count)
                out.push_back({});
            else
                out.push_back(static_cast<std::int64_t>(h));
        }
    }
    return out;
}

constexpr Frame HASH_FRAMES[] = {
    {"rows 2", 1, 0, WindowFrameMode::Rows},
    {"rows 10", 9, 0, WindowFrameMode::Rows},
    {"rows 21 around", 9, 11, WindowFrameMode::Rows},
    {"rows 64", 63, 0, WindowFrameMode::Rows},
    {"rows 11 ahead", 0, 10, WindowFrameMode::Rows},
    {"rows 500 back 7 ahead", 499, 7, WindowFrameMode::Rows},
    {"rows running", UNBOUNDED, 0, WindowFrameMode::Rows},
    {"rows to end", 0, UNBOUNDED, WindowFrameMode::Rows},
    {"rows whole", UNBOUNDED, UNBOUNDED, WindowFrameMode::Rows},
};

}  // namespace

TEST_CASE(
    "an order-sensitive UDAF without retract merges frames in row order") {
    auto set = load();
    for (const std::int64_t parts : {1, 3}) {
        const DataFrame df = make_big(3000, parts);
        for (const Frame& f : HASH_FRAMES) {
            for (const std::int64_t min_count : {0, 1, 5}) {
                INFO(parts);
                INFO(f.label);
                INFO(min_count);
                const DataFrame out =
                    run_on(df, {udaf_of("uhash", "h", min_count, f.preceding,
                                        f.following, f.mode)});
                const Cells want = hash_expected(df, parts, f.preceding,
                                                 f.following, min_count);
                const Cells got = cells(out, "h");
                REQUIRE(got.size() == want.size());
                std::size_t wrong = 0;
                for (std::size_t r = 0; r < want.size(); ++r)
                    if (got[r] != want[r]) ++wrong;
                CHECK(wrong == 0);
            }
        }
    }
}

TEST_CASE("a tree window whose frames all fall short of min_count is null") {
    auto set = load();
    const std::int64_t rows = 300;
    const DataFrame df = make_big(rows, 2);
    for (const char* name : {"usum", "uhash"}) {
        INFO(name);
        const DataFrame out =
            run_on(df, {udaf_of(name, "x", 50, 20, 0, WindowFrameMode::Rows)});
        const Cells got = cells(out, "x");
        REQUIRE(static_cast<std::int64_t>(got.size()) == rows);
        for (const auto& cell : got) CHECK_FALSE(cell.has_value());
    }

    dstd::Vector<std::int64_t> p(rows, 0), o, v(rows, 7);
    for (std::int64_t i = 0; i < rows; ++i) o.push_back(i);
    const dstd::Vector<std::uint8_t> none(
        static_cast<std::size_t>(rows + 7) / 8, 0);
    DataFrame empty;
    empty.names.push_back(dstd::SharedString{"p"});
    empty.names.push_back(dstd::SharedString{"o"});
    empty.names.push_back(dstd::SharedString{"v"});
    empty.columns.push_back(Series::flat_i64(p.data(), rows, nullptr));
    empty.columns.push_back(Series::flat_i64(o.data(), rows, nullptr));
    empty.columns.push_back(Series::flat_i64(v.data(), rows, none.data()));
    for (const char* name : {"usum", "uhash"}) {
        INFO(name);
        const DataFrame out = run_on(
            empty, {udaf_of(name, "x", 0, 20, 0, WindowFrameMode::Rows)});
        const Cells got = cells(out, "x");
        REQUIRE(static_cast<std::int64_t>(got.size()) == rows);
        for (const auto& cell : got) CHECK_FALSE(cell.has_value());
    }
}

TEST_CASE("the tree of a window holds only the levels its frames can use") {
    auto set = load();
    const std::int64_t rows = 4096;
    const DataFrame df = make_big(rows, 1);
    const std::int64_t inits = counter("udaf_plugin_inits");
    const DataFrame out =
        run_on(df, {udaf_of("uhash", "h", 0, 9, 0, WindowFrameMode::Rows)});
    CHECK(cells(out, "h") == hash_expected(df, 1, 9, 0, 0));
    // A frame of 10 rows takes nodes of 1, 2, 4 and 8 rows; each of the rows
    // also gets a state of its own for its frame.
    const std::int64_t nodes = rows + rows / 2 + rows / 4 + rows / 8;
    CHECK(counter("udaf_plugin_inits") - inits == nodes + rows);
}

TEST_CASE("a sliding UDAF over frames reaching the end of a long partition") {
    auto set = load();
    const DataFrame df = make_big(600000, 1);
    for (const Frame& f :
         {Frame{"to end", 0, UNBOUNDED, WindowFrameMode::Rows},
          Frame{"whole", UNBOUNDED, UNBOUNDED, WindowFrameMode::Rows},
          Frame{"last 400000", 399999, UNBOUNDED, WindowFrameMode::Rows}}) {
        INFO(f.label);
        const DataFrame out = run_on(
            df,
            {sum_of("b", 1, f.preceding, f.following, f.mode),
             udaf_of("usumr", "slide", 1, f.preceding, f.following, f.mode)});
        CHECK(cells(out, "slide") == cells(out, "b"));
    }
}

#ifdef STATS_UDAFS_PLUGIN_PATH
TEST_CASE("p50 over frames equals the element at count / 2 of the frame") {
    auto built = plugins::host::Plugins::builder()
                     .add(UDAF_PLUGIN_PATH)
                     .add(STATS_UDAFS_PLUGIN_PATH)
                     .build();
    REQUIRE(built.has_value());
    const DataFrame df = make_big(4200, 2);
    for (const Frame& f : BIG_FRAMES) {
        if (f.mode != WindowFrameMode::Rows) continue;
        for (const std::int64_t min_count : {0, 3}) {
            INFO(f.label);
            INFO(min_count);
            const DataFrame out =
                run_on(df, {udaf_of("p50", "m", min_count, f.preceding,
                                    f.following, f.mode)});
            const DoubleCells want =
                p50_expected(df, 2, f.preceding, f.following, min_count);
            const DoubleCells got = double_cells(out, "m");
            REQUIRE(got.size() == want.size());
            std::size_t wrong = 0;
            for (std::size_t r = 0; r < want.size(); ++r)
                if (got[r] != want[r]) ++wrong;
            CHECK(wrong == 0);
        }
    }
}

TEST_CASE("p50 over a window whose states span several chunks is exact") {
    auto built = plugins::host::Plugins::builder()
                     .add(UDAF_PLUGIN_PATH)
                     .add(STATS_UDAFS_PLUGIN_PATH)
                     .build();
    REQUIRE(built.has_value());
    const DataFrame df = make_big(30000, 1);
    const DataFrame out =
        run_on(df, {udaf_of("p50", "m", 1, 199, 0, WindowFrameMode::Rows)});
    CHECK(double_cells(out, "m") == p50_expected(df, 1, 199, 0, 1));
}
#endif
