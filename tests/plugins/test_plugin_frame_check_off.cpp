// SPDX-License-Identifier: MIT
// The same fixture as test_plugin_frame_check, with DFTRACER_UTILS_PLUGIN_CHECK
// unset: the host validates nothing, so a broken frame reaches the caller
// unchanged. That is the contract the check has to earn - off costs nothing
// and changes no answer - and this file is the proof that the case above fails
// because of the check, not because the fixture cannot produce rows.

#define DOCTEST_CONFIG_IMPLEMENT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/str_format.h>
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

dstd::Optional<plugins::host::Plugins> load_fixture() {
    auto built =
        plugins::host::Plugins::Builder().add(BROKEN_FRAME_PLUGIN_PATH).build();
    if (!built) {
        MESSAGE("load failed: " << built.error().message());
        return dstd::nullopt;
    }
    return std::move(built).value();
}

// A file the fixture's format claims by its magic.
dstd::String write_sample() {
    const char* const path = "broken_frame_sample_off.bfr";
    std::FILE* f = std::fopen(path, "wb");
    REQUIRE(f != nullptr);
    std::fputs("BFRAME\none\ntwo\n", f);
    std::fclose(f);
    return dstd::String(path);
}

}  // namespace

TEST_CASE("with check mode off the broken provider frame is passed through") {
    auto set = load_fixture();
    REQUIRE(set.has_value());
    auto factory = find_provider("broken_frame.bad_column");
    REQUIRE(factory != nullptr);
    LazyFrame plan = LazyFrame::scan(factory->make_source());

    auto got = default_runtime().submit(plan.collect()).get();
    if (!got) MESSAGE("the scan failed: " << got.error().message());
    REQUIRE(got.has_value());
    CHECK(got->num_rows() == 2);
}

TEST_CASE("with check mode off the broken UDF result is passed through") {
    auto set = load_fixture();
    REQUIRE(set.has_value());
    auto call = udf("broken_frame.bad_udf");
    REQUIRE(call.has_value());

    const std::int64_t xs[] = {1, 2, 3};
    const Series in = Series::flat_i64(xs, 3);
    auto got = eval((*call)(col(0)), {&in});
    if (!got) MESSAGE("the UDF failed: " << got.error().message());
    REQUIRE(got.has_value());
    CHECK(got->length() == 3);
}

TEST_CASE("with check mode off the broken node frame is passed through") {
    auto set = load_fixture();
    REQUIRE(set.has_value());
    auto factory = find_provider("broken_frame.good_column");
    REQUIRE(factory != nullptr);
    LazyFrame plan = LazyFrame::scan(factory->make_source())
                         .op("broken_frame.bad_node", OpArgs());

    auto got = default_runtime().submit(plan.collect()).get();
    if (!got) MESSAGE("the scan failed: " << got.error().message());
    REQUIRE(got.has_value());
    CHECK(got->num_rows() == 2);
}

TEST_CASE("with check mode off the broken format frame is passed through") {
    auto set = load_fixture();
    REQUIRE(set.has_value());
    const dstd::String path = write_sample();

    auto got =
        default_runtime().submit(LazyFrame::scan_file(path).collect()).get();
    std::remove(path.c_str());
    if (!got) MESSAGE("the scan failed: " << got.error().message());
    REQUIRE(got.has_value());
    CHECK(got->num_rows() == 2);
}

TEST_CASE("with check mode off the broken UDAF result is passed through") {
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
    if (!got) MESSAGE("finalize failed: " << got.error().message());
    REQUIRE(got.has_value());
    CHECK(got->length() == 1);
}

TEST_CASE("with check mode off the broken op result is passed through") {
    auto set = load_fixture();
    REQUIRE(set.has_value());
    const dftu_op_desc* op = dftu_op_find("broken_frame.bad_op");
    REQUIRE(op != nullptr);
    const std::int64_t xs[] = {1, 2, 3};
    const Series in = Series::flat_i64(xs, 3);
    const dftu_series* ins[] = {in.handle()};

    dftu_series* got = dftu_op_run(op, ins, 1, nullptr);
    REQUIRE(got != nullptr);
    CHECK(dftu_series_length(got) == 3);
    dftu_series_free(got);
    CHECK(op_check_error().empty());
}

TEST_CASE("with check mode off the broken transform output is passed on") {
    auto set = load_fixture();
    REQUIRE(set.has_value());
    dftu_utils_test::TestEnvironment env(8);
    dstd::String dir = env.dir() + "/t";
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    dstd::String pfw = dir + "/trace.pfw";
    dstd::String ofs;
    for (int i = 0; i < 8; ++i)
        ofs +=
            common::str_cat(R"({"ph":"X","name":"read","cat":"POSIX","pid":1)",
                            R"(,"tid":1,"ts":)", (1000 + i * 100), R"(,"dur":)",
                            (10 + i), R"(,"args":{}})", "\n");
    dstd::fs::write_file(dstd::PathView(pfw), ofs).discard();
    dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dstd::fs::remove(pfw).discard();
    ViewFile file = test_plugin_common::index_trace(gz);
    View view = View::from_files(dstd::Vector<ViewFile>{file});

    Runtime rt(4);
    dstd::String why;
    auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
        auto run = co_await set->run(view);
        if (!run) why = dstd::String(run.error().message());
        co_return;
    });
    dftu_utils_test::ok(rt.submit(std::move(task), "plugin-check-off").wait());
    rt.shutdown();
    if (!why.empty()) MESSAGE("the scan failed: " << why);
    CHECK(why.empty());
}

int main(int argc, char** argv) {
    ::unsetenv(common::constants::plugins::CHECK_ENV);
    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    return ctx.run();
}
