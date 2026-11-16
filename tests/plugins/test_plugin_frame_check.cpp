// SPDX-License-Identifier: MIT
// With DFTRACER_UTILS_PLUGIN_CHECK on, the host validates every frame and
// column a plugin hands back and fails the run naming the plugin, the callback
// and the rule. This file drives a fixture that returns a broken column from a
// provider cursor, a UDF exec, a node cursor, a file format cursor, a UDAF
// finalize, a column op and a fold transform. The flag is read once and cached,
// so it is set in main, before the first read; test_plugin_frame_check_off runs
// the same fixture with the flag unset.

#define DOCTEST_CONFIG_IMPLEMENT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/provider_registry.h>
#include <dftracer/utils/dataframe/internal/validate.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/udaf.h>
#include <dftracer/utils/dataframe/udaf_registry.h>
#include <dftracer/utils/dataframe/udaf_states.h>
#include <dftracer/utils/dataframe/udf.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <plugins/test_plugin_common.h>
#include <testing_utilities.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

using View = dftracer::utils::trace::views::View;
using ViewFile = dftracer::utils::trace::views::ViewFile;

#ifndef BROKEN_FRAME_PLUGIN_PATH
#error "BROKEN_FRAME_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

bool says(dstd::StringView text, dstd::StringView part) {
    return text.find(part) != dstd::StringView::npos;
}

dstd::Optional<plugins::host::Plugins> load_fixture() {
    auto built =
        plugins::host::Plugins::Builder().add(BROKEN_FRAME_PLUGIN_PATH).build();
    if (!built) {
        MESSAGE("load failed: " << built.error().message());
        return dstd::nullopt;
    }
    return std::move(built).value();
}

// The plan over a provider the fixture registers, or none. `name` picks a good
// one, for a test that needs a sound input under a broken stage.
dstd::Optional<LazyFrame> provider_plan(dstd::StringView name) {
    auto factory = find_provider(name);
    if (!factory) {
        MESSAGE("the fixture registered no " << name << " provider");
        return dstd::nullopt;
    }
    return LazyFrame::scan(factory->make_source());
}

// A file the fixture's format claims by its magic.
dstd::String write_sample() {
    const char* const path = "broken_frame_sample.bfr";
    std::FILE* f = std::fopen(path, "wb");
    REQUIRE(f != nullptr);
    std::fputs("BFRAME\none\ntwo\n", f);
    std::fclose(f);
    return dstd::String(path);
}

// One trace the fixture's fold transforms, so its transform output reaches the
// shared batch bus a later step would read.
dstd::String make_trace(dftu_utils_test::TestEnvironment& env) {
    dstd::String dir = env.dir() + "/t";
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    dstd::String pfw = dir + "/trace.pfw";
    dstd::String ofs;
    for (int i = 0; i < 8; ++i)
        dstd::fmt_append<
            "{\"ph\":\"X\",\"name\":\"read\",\"cat\":\"POSIX\",\"pid\":1,"
            "\"tid\":1,\"ts\":{},\"dur\":{}{}\n">(ofs, (1000 + i * 100),
                                                  (10 + i), R"(,"args":{}})");
    dstd::fs::write_file(dstd::PathView(pfw), ofs).discard();
    dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dstd::fs::remove(pfw).discard();
    return gz;
}

}  // namespace

TEST_CASE("a broken provider frame fails the scan naming plugin, callback") {
    auto set = load_fixture();
    REQUIRE(set.has_value());
    auto plan = provider_plan("broken_frame.bad_column");
    REQUIRE(plan.has_value());

    auto got = default_runtime().submit(plan->collect()).get();
    if (got) MESSAGE("the scan of a broken frame succeeded");
    REQUIRE_FALSE(got.has_value());
    const dstd::StringView why = got.error().message();
    MESSAGE("error: " << why);
    CHECK(says(why, "broken_frame.bad_column"));
    CHECK(says(why, "next returned a broken frame"));
    CHECK(says(why, "column 'name'"));
    CHECK(says(why, "nulls but no validity bitmap"));
}

TEST_CASE("a broken UDF result fails the query naming plugin, callback") {
    auto set = load_fixture();
    REQUIRE(set.has_value());
    auto call = udf("broken_frame.bad_udf");
    REQUIRE(call.has_value());

    const std::int64_t xs[] = {1, 2, 3};
    const Series in = Series::flat_i64(xs, 3);
    auto got = eval((*call)(col(0)), {&in});
    if (got) MESSAGE("the UDF over a broken result succeeded");
    REQUIRE_FALSE(got.has_value());
    const dstd::StringView why = got.error().message();
    MESSAGE("error: " << why);
    CHECK(says(why, "broken_frame.bad_udf"));
    CHECK(says(why, "exec returned a broken column"));
    CHECK(says(why, "nulls but no validity bitmap"));
}

TEST_CASE("a broken node frame fails the scan naming node, callback, rule") {
    auto set = load_fixture();
    REQUIRE(set.has_value());
    auto base = provider_plan("broken_frame.good_column");
    REQUIRE(base.has_value());
    LazyFrame plan = base->op("broken_frame.bad_node", OpArgs());

    auto got = default_runtime().submit(plan.collect()).get();
    if (got) MESSAGE("the scan over a broken node frame succeeded");
    REQUIRE_FALSE(got.has_value());
    const dstd::StringView why = got.error().message();
    MESSAGE("error: " << why);
    CHECK(says(why, "broken_frame.bad_node"));
    CHECK(says(why, "next returned a broken frame"));
    CHECK(says(why, "nulls but no validity bitmap"));
}

TEST_CASE("a broken file format frame fails the scan naming the format") {
    auto set = load_fixture();
    REQUIRE(set.has_value());
    const dstd::String path = write_sample();

    auto got =
        default_runtime().submit(LazyFrame::scan_file(path).collect()).get();
    std::remove(path.c_str());
    if (got) MESSAGE("the scan over a broken format frame succeeded");
    REQUIRE_FALSE(got.has_value());
    const dstd::StringView why = got.error().message();
    MESSAGE("error: " << why);
    CHECK(says(why, "broken_frame.bad_format"));
    CHECK(says(why, "next returned a broken frame"));
    CHECK(says(why, "nulls but no validity bitmap"));
}

TEST_CASE("a broken UDAF finalize result fails naming the aggregate") {
    auto set = load_fixture();
    REQUIRE(set.has_value());
    auto record = find_udaf("broken_frame.bad_udaf");
    REQUIRE(record);
    const DataType type = scalar(TypeId::Int64);
    auto bound =
        bind_udaf(std::move(record), dstd::Span<const DataType>(&type, 1), {});
    REQUIRE(bound.has_value());
    UdafStates table(std::move(*bound));
    REQUIRE(table.grow(1).has_value());
    const std::int64_t xs[] = {1, 2, 3};
    const Series col_xs = Series::flat_i64(xs, 3);
    const Series* args[] = {&col_xs};
    REQUIRE(
        table.update(dstd::Vector<std::int64_t>{0, 0, 0}, args).has_value());

    auto got = table.finalize();
    if (got) MESSAGE("the broken finalize result was accepted");
    REQUIRE_FALSE(got.has_value());
    const dstd::StringView why = got.error().message();
    MESSAGE("error: " << why);
    CHECK(says(why, "broken_frame.bad_udaf"));
    CHECK(says(why, "finalize returned a broken column"));
    CHECK(says(why, "nulls but no validity bitmap"));
}

TEST_CASE("a broken plugin op result is refused naming the op and the rule") {
    auto set = load_fixture();
    REQUIRE(set.has_value());
    const dftu_op_desc* op = dftu_op_find("broken_frame.bad_op");
    REQUIRE(op != nullptr);
    const std::int64_t xs[] = {1, 2, 3};
    const Series in = Series::flat_i64(xs, 3);
    const dftu_series* ins[] = {in.handle()};

    CHECK(dftu_op_run(op, ins, 1, nullptr) == nullptr);
    const dstd::StringView why = op_check_error();
    MESSAGE("error: " << why);
    CHECK(says(why, "op 'broken_frame.bad_op'"));
    CHECK(says(why, "returned a broken column"));
    CHECK(says(why, "nulls but no validity bitmap"));
}

TEST_CASE("a broken transform output fails the scan naming plugin, callback") {
    auto set = load_fixture();
    REQUIRE(set.has_value());
    dftu_utils_test::TestEnvironment env(8);
    ViewFile file = test_plugin_common::index_trace(make_trace(env));
    View view = View::from_files(dstd::Vector<ViewFile>{file});
    Runtime rt(4);
    dstd::String why;
    auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
        auto run = co_await set->run(view);
        if (!run) why = dstd::String(run.error().message());
        co_return;
    });
    dftu_utils_test::ok(
        rt.submit(std::move(task), "plugin-frame-check").wait());
    rt.shutdown();
    if (why.empty())
        MESSAGE("the scan over a broken transform output succeeded");
    REQUIRE_FALSE(why.empty());
    MESSAGE("error: " << why);
    CHECK(says(why, "plugin 'broken_frame_plugin'"));
    CHECK(says(why, "transform"));
    CHECK(says(why, "nulls but no validity bitmap"));
}

int main(int argc, char** argv) {
    ::setenv(common::constants::plugins::CHECK_ENV, "1", 1);
    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    return ctx.run();
}
