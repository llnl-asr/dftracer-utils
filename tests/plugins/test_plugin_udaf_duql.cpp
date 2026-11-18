// SPDX-License-Identifier: MIT
// Registered UDAFs in duql: a `group` or `agg` block calls one by name like a
// built-in aggregate, over the engine's group-by or the streaming group fold,
// with the argument count checked when the query compiles, the failure message
// of the UDAF in the query error, and a UDAF without DFTU_AGG_PARALLEL_SAFE
// never entered by two threads.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/udaf_registry.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <dlfcn.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <algorithm>
#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::views;

#ifndef UDAF_PLUGIN_PATH
#error "UDAF_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

constexpr int KEYS = 4;

dstd::String event(int i) {
    return R"({"ph":"X","name":"op)" + dstd::to_string(i % KEYS) +
           R"(","cat":"POSIX","pid":1,"tid":1,"ts":)" +
           dstd::to_string(1000 + 100 * i) + R"(,"dur":)" +
           dstd::to_string(10 + i) + R"(,"args":{}})";
}

// Events `first` to `first + rows - 1` in a trace file of their own.
ViewFile file_of(dftu_utils_test::TestEnvironment& env, int first, int rows) {
    const dstd::String dir = env.dir() + "/f" + dstd::to_string(first);
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String pfw = dir + "/trace.pfw";
    {
        dftu_utils_test::FileOut o(pfw);
        for (int i = first; i < first + rows; ++i) o << event(i) << "\n";
    }
    const dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dstd::fs::remove(pfw).discard();
    dftu_utils_test::ok(
        dftu_utils_test::ok(
            trace::index::Indexer::open({dstd::String(dstd::StringView(gz))}))
            .build());
    ViewFile f;
    f.file_path = gz;
    f.index_path = trace::internal::determine_index_path(gz, "");
    return f;
}

View view_of(dftu_utils_test::TestEnvironment& env, int files,
             int rows_per_file) {
    dstd::Vector<ViewFile> list;
    for (int f = 0; f < files; ++f)
        list.push_back(file_of(env, f * rows_per_file, rows_per_file));
    return View::from_files(std::move(list));
}

dstd::Result<dataframe::DataFrame> collect(const View& v) {
    return default_runtime().submit(v.collect()).get();
}

dstd::Vector<dstd::String> cells(const dataframe::DataFrame& f,
                                 dstd::CStringView name) {
    std::size_t c = 0;
    while (c < f.names.size() && f.names[c] != name) ++c;
    REQUIRE(c < f.names.size());
    const dataframe::Series col = f.columns[c].materialize();
    dstd::Vector<dstd::String> out;
    for (std::int64_t r = 0; r < f.num_rows(); ++r) {
        if (col.is_null(r)) {
            out.emplace_back("null");
            continue;
        }
        switch (col.type()) {
            case dataframe::TypeId::Int64:
                out.push_back(dstd::to_string(col.data<std::int64_t>()[r]));
                break;
            case dataframe::TypeId::Uint64:
                out.push_back(dstd::to_string(col.data<std::uint64_t>()[r]));
                break;
            case dataframe::TypeId::Float64:
                out.push_back(dstd::to_string(col.data<double>()[r]));
                break;
            default:
                out.emplace_back(col.string_at(r));
        }
    }
    return out;
}

dstd::String joined(dstd::Span<const dstd::String> list) {
    dstd::String out;
    for (const auto& s : list) out += s + " ";
    return out;
}

dataframe::DataFrame run(const View& v, dstd::StringView query) {
    const View q = v.duql(query);
    auto got = collect(q);
    INFO(query);
    INFO(
        (got.has_value() ? dstd::String{} : dstd::String(got.error().c_str())));
    REQUIRE(got.has_value());
    return std::move(*got);
}

// The failure message of the query, from building the view or running it.
dstd::String failure(const View& v, dstd::StringView query) {
    const View q = v.duql(query);
    if (const auto kept = q.status(); !kept.has_value())
        return kept.error().c_str();
    const auto got = collect(q);
    if (!got.has_value()) return got.error().c_str();
    return {};
}

bool has(dstd::StringView text, dstd::StringView word) {
    return text.find(word) != dstd::String::npos;
}

dstd::Optional<plugins::host::Plugins> load(dstd::StringView path) {
    auto built =
        plugins::host::Plugins::builder().add(dstd::String(path)).build();
    INFO((built.has_value() ? dstd::String{}
                            : dstd::String(built.error().message())));
    REQUIRE(built.has_value());
    return std::move(*built);
}

void* plugin_symbol(dstd::CStringView symbol) {
    void* h = dlopen(UDAF_PLUGIN_PATH, RTLD_NOW | RTLD_NOLOAD);
    REQUIRE(h != nullptr);
    void* fn = dlsym(h, symbol.c_str());
    dlclose(h);
    REQUIRE(fn != nullptr);
    return fn;
}

std::int64_t max_active() {
    return reinterpret_cast<std::int64_t (*)()>(
        plugin_symbol("udaf_plugin_max_active"))();
}

void reset_active() {
    reinterpret_cast<void (*)()>(plugin_symbol("udaf_plugin_reset_active"))();
}

}  // namespace

TEST_CASE("a group block calls a UDAF like the built-in aggregate it mirrors") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of(env, 1, 40);
    auto set = load(UDAF_PLUGIN_PATH);

    // Both aggregates the engine runs.
    const auto engine =
        run(v, "group name { s = sum(dur), u = usum(dur) } | sort name");
    REQUIRE(engine.num_rows() == KEYS);
    CHECK(engine.columns[2].type() == dataframe::TypeId::Int64);
    CHECK(cells(engine, "u") == cells(engine, "s"));
    CHECK(cells(engine, "u")[0] == "280");

    // count_distinct makes the group fold run them.
    const auto folded =
        run(v,
            "group name { s = sum(dur), u = usum(dur), "
            "d = count_distinct(dur), ud = udistinct(int(dur)) } | "
            "sort name");
    REQUIRE(folded.num_rows() == KEYS);
    CHECK(cells(folded, "u") == cells(folded, "s"));
    CHECK(cells(folded, "ud") == cells(folded, "d"));
    CHECK(cells(folded, "ud")[0] == "10");
    CHECK(cells(folded, "u") == cells(engine, "u"));

    // No key: one row, over the whole input.
    const auto all = run(v, "agg { s = sum(dur), u = usum(dur) }");
    CHECK(cells(all, "u") == cells(all, "s"));
    CHECK(cells(all, "u")[0] == "1180");

    // In an expression, the UDAF fills a hidden column.
    const auto scaled = run(v, "group name { u = usum(dur) * 2 } | sort name");
    CHECK(cells(scaled, "u")[0] == "560");
}

TEST_CASE("a UDAF reads several files the same as one") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of(env, 5, 600);
    auto set = load(UDAF_PLUGIN_PATH);

    for (const char* extra : {"", ", d = count_distinct(dur)"}) {
        const auto out =
            run(v, dstd::String("group name { s = sum(dur), u = "
                                "usum(dur), ud = udistinct(int(dur))") +
                       extra + " } | sort name");
        REQUIRE(out.num_rows() == KEYS);
        CHECK(cells(out, "u") == cells(out, "s"));
        CHECK(cells(out, "ud")[0] == "750");
    }
}

#ifdef STATS_UDAFS_PLUGIN_PATH
TEST_CASE("p50 of the stats plugin gives the element at n / 2 of each group") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of(env, 1, 40);
    auto set = load(STATS_UDAFS_PLUGIN_PATH);

    dstd::Vector<dstd::String> want;
    for (int k = 0; k < KEYS; ++k) {
        dstd::Vector<double> d;
        for (int i = k; i < 40; i += KEYS) d.push_back(10.0 + i);
        std::sort(d.begin(), d.end());
        want.push_back(dstd::to_string(d[d.size() / 2]));
    }
    const auto out = run(v, "group name { m = p50(dur) } | sort name");
    CHECK(out.columns[1].type() == dataframe::TypeId::Float64);
    CHECK(cells(out, "m") == want);
    const auto folded =
        run(v,
            "group name { m = p50(dur), d = count_distinct(dur) } | "
            "sort name");
    CHECK(cells(folded, "m") == want);
}
#endif

TEST_CASE("a UDAF call is checked when the query compiles") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of(env, 1, 40);
    auto set = load(UDAF_PLUGIN_PATH);

    const dstd::String many = failure(v, "group name { u = usum(dur, ts) }");
    CHECK(has(many, "usum"));
    CHECK(has(many, "argument"));
    const dstd::String none = failure(v, "agg { u = usum() }");
    CHECK(has(none, "usum"));
    CHECK(has(none, "argument"));

    // udistinct takes Int64 only, and dur is not.
    const dstd::String type = failure(v, "agg { u = udistinct(dur) }");
    CHECK(has(type, "udistinct"));
    CHECK(has(type, "not accepted"));

    const dstd::String constant = failure(v, "group name { u = usum(1) }");
    CHECK(has(constant, "usum"));
    CHECK(has(constant, "constant"));

    const dstd::String unknown =
        failure(v, "group name { u = nosuchagg(dur) }");
    CHECK(has(unknown, "Unknown function"));
    CHECK(has(unknown, "nosuchagg"));

    const dstd::String outside = failure(v, "derive u = usum(dur) | select u");
    CHECK(has(outside, "usum"));
    CHECK(has(outside, "user-defined aggregate"));

    const dstd::String pivot = failure(v, "pivot name { u = usum(dur) }");
    CHECK(has(pivot, "usum"));
    CHECK(has(pivot, "user-defined aggregate"));
}

TEST_CASE("without the plugin loaded the UDAF name is unknown") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of(env, 1, 40);
    const dstd::String why = failure(v, "group name { u = usum(dur) }");
    CHECK(has(why, "Unknown function"));
    CHECK(has(why, "usum"));
}

TEST_CASE("a failing UDAF reaches the query error with its message") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of(env, 1, 40);
    auto set = load(UDAF_PLUGIN_PATH);

    for (const char* extra : {"", ", d = count_distinct(dur)"}) {
        const dstd::String why = failure(
            v, dstd::String("group name { f = ufail(dur)") + extra + " }");
        CHECK(has(why, "ufail"));
        CHECK(has(why, "refused on purpose"));
    }
    const dstd::String bad = failure(v, "agg { f = ubadrows(dur) }");
    CHECK_FALSE(bad.empty());
}

TEST_CASE("a UDAF without the parallel flag is never entered by two threads") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of(env, 6, 3000);
    auto set = load(UDAF_PLUGIN_PATH);

    for (const char* extra : {"", ", d = count_distinct(dur)"}) {
        reset_active();
        const auto out =
            run(v, dstd::String("group name { s = sum(dur), u = userial(dur)") +
                       extra + " } | sort name");
        CHECK(max_active() == 1);
        CHECK(cells(out, "u") == cells(out, "s"));
    }
}

TEST_CASE(
    "a lookup side that calls a UDAF is never stored in the lookup cache") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of(env, 1, 40);
    auto set = load(UDAF_PLUGIN_PATH);

    const auto side = [](dstd::CStringView fn) {
        return dstd::String("let g = group name { u = ") + fn.c_str() +
               "(dur) }; lookup g on name | select name, u";
    };
    auto plan = [&](dstd::StringView q) {
        auto text = v.explain_duql(q);
        REQUIRE(text.has_value());
        return *text;
    };
    // A built-in side is cached by its first run, so the UDAF side below
    // staying uncached comes from the rule and not from the query shape.
    const dstd::String builtin = side("sum");
    CHECK(has(plan(builtin), "not cached"));
    run(v, builtin);
    CHECK(has(plan(builtin), "cached, 4 rows"));

    const dstd::String udaf = side("usum");
    CHECK(has(plan(udaf), "not cached"));
    const auto out = run(v, udaf);
    CHECK(out.num_rows() == 40);
    CHECK(has(plan(udaf), "not cached"));
    CHECK_FALSE(has(plan(udaf), "cached, "));
}

TEST_CASE("a UDAF of two arguments and one of none run in a group block") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of(env, 3, 200);
    auto set = load(UDAF_PLUGIN_PATH);

    for (const char* extra : {"", ", d = count_distinct(dur)"}) {
        INFO("block extra: " << extra);
        const auto out =
            run(v, dstd::String("group name { s = sum(int(dur) * int(ts)), w = "
                                "uwsum(int(dur), int(ts)), n = count(), c = "
                                "ucount()") +
                       extra + " } | sort name");
        REQUIRE(out.num_rows() == KEYS);
        CHECK(cells(out, "w") == cells(out, "s"));
        CHECK(cells(out, "c") == cells(out, "n"));
        CHECK(cells(out, "c")[0] == "150");

        // Swapped arguments give the same product; a different pair does not.
        const auto other = run(
            v, dstd::String("group name { w = uwsum(int(dur), int(ts)), x = "
                            "uwsum(int(ts), int(dur)), y = uwsum(int(dur), "
                            "int(dur))") +
                   extra + " } | sort name");
        CHECK(cells(other, "x") == cells(other, "w"));
        CHECK(cells(other, "y") != cells(other, "w"));
    }

    const auto all = run(v, "agg { c = ucount(), n = count() }");
    CHECK(cells(all, "c") == cells(all, "n"));
    CHECK(cells(all, "c")[0] == "600");
}

TEST_CASE(
    "a window block calls a UDAF over a rows frame, a range frame or all") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of(env, 1, 40);
    auto set = load(UDAF_PLUGIN_PATH);

    // usumr slides its frame, usum recomputes it; both equal the built-in sum.
    const auto rows =
        run(v,
            "window name sort ts { b = sum(dur) over 3 rows, "
            "s = usumr(dur) over 3 rows, r = usum(dur) over 3 rows }");
    REQUIRE(rows.num_rows() == 40);
    CHECK(joined(cells(rows, "s")) == joined(cells(rows, "b")));
    CHECK(joined(cells(rows, "r")) == joined(cells(rows, "b")));
    CHECK(cells(rows, "s")[0] == "10");
    // Row 8 is the third of op0: 18 + 14 + 10.
    CHECK(cells(rows, "s")[8] == "42");

    // Rows of one name are 400 apart in ts, so 900 holds three of them.
    const auto range = run(v,
                           "window name sort ts { b = sum(dur) over 900, "
                           "s = usumr(dur) over 900, r = usum(dur) over 900 }");
    CHECK(joined(cells(range, "s")) == joined(cells(range, "b")));
    CHECK(joined(cells(range, "r")) == joined(cells(range, "b")));
    CHECK(joined(cells(range, "s")) == joined(cells(rows, "s")));

    // No frame: the whole partition, as a group gives it.
    const auto whole = run(v, "window name { u = usum(dur), b = sum(dur) }");
    CHECK(joined(cells(whole, "u")) == joined(cells(whole, "b")));
    CHECK(cells(whole, "u")[0] == "280");
    const auto grouped = run(v, "group name { u = usum(dur) } | sort name");
    CHECK(cells(grouped, "u")[0] == cells(whole, "u")[0]);
    CHECK(cells(grouped, "u")[1] == cells(whole, "u")[1]);
}

#ifdef STATS_UDAFS_PLUGIN_PATH
TEST_CASE("p50 over a rows frame recomputes each frame") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of(env, 1, 40);
    auto set = load(STATS_UDAFS_PLUGIN_PATH);

    // Each name's durations rise, so a frame of up to three holds the element
    // at n / 2: the row itself for one or two rows, the row before for three.
    dstd::Vector<dstd::String> want;
    for (int i = 0; i < 40; ++i) {
        const int j = i / KEYS;
        want.push_back(dstd::to_string(10.0 + (j <= 1 ? i : i - KEYS)));
    }
    const auto out = run(v, "window name sort ts { m = p50(dur) over 3 rows }");
    CHECK(out.columns.back().type() == dataframe::TypeId::Float64);
    CHECK(cells(out, "m") == want);

    const auto whole = run(v, "window name { m = p50(dur) }");
    const auto grouped = run(v, "group name { m = p50(dur) } | sort name");
    CHECK(cells(whole, "m")[0] == cells(grouped, "m")[0]);
}
#endif

TEST_CASE("a window UDAF reads the column only its second argument names") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of(env, 1, 40);
    auto set = load(UDAF_PLUGIN_PATH);

    // The select closes the pipeline, so the scan reads only the fields the
    // stages name; pid is named only as the second argument and is 1 in every
    // event.
    const auto out = run(v,
                         "window name sort ts { b = sum(int(dur)) over 3 rows, "
                         "w = uwsum(int(dur), int(pid)) over 3 rows } | "
                         "select name, ts, b, w");
    REQUIRE(out.num_rows() == 40);
    CHECK(joined(cells(out, "w")) == joined(cells(out, "b")));
    CHECK(cells(out, "w")[0] == "10");
}

TEST_CASE("a UDAF window call is checked when the query compiles") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of(env, 1, 40);
    auto set = load(UDAF_PLUGIN_PATH);

    const dstd::String many =
        failure(v, "window name sort ts { u = usum(dur, ts) over 3 rows }");
    CHECK(has(many, "usum"));
    CHECK(has(many, "argument"));
    const dstd::String constant =
        failure(v, "window name sort ts { u = usum(1) over 3 rows }");
    CHECK(has(constant, "usum"));
    CHECK(has(constant, "constant"));
    const dstd::String no_sort =
        failure(v, "window name { u = usum(dur) over 5 }");
    CHECK(has(no_sort, "sort"));
    const dstd::String fails =
        failure(v, "window name sort ts { u = ufail(dur) over 3 rows }");
    CHECK(has(fails, "refused on purpose"));
    const dstd::String retract =
        failure(v, "window name sort ts { u = uretfail(dur) over 3 rows }");
    CHECK(has(retract, "retract refused"));
}

TEST_CASE("a UDAF window over several files equals the built-in sum") {
    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const View v = view_of(env, 5, 600);
    auto set = load(UDAF_PLUGIN_PATH);

    const auto out =
        run(v,
            "window name sort ts { b = sum(dur) over 4 rows, "
            "s = usumr(dur) over 4 rows, r = usum(dur) over 4 rows }");
    REQUIRE(out.num_rows() == 3000);
    CHECK(joined(cells(out, "s")) == joined(cells(out, "b")));
    CHECK(joined(cells(out, "r")) == joined(cells(out, "b")));
}
