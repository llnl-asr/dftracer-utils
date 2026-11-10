// SPDX-License-Identifier: MIT
// dftu_plugin::reads projects the batch down to the columns a plugin declares.
// Declaring none still materializes every fixed column plus one Series per
// distinct arg key in the batch - the cost this feature exists to avoid.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/plugins/config.h>
#include <dftracer/utils/plugins/plugins.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <utilities/plugins/test_plugin_common.h>

using namespace dftracer::utils;

using View = dftracer::utils::trace::views::View;
using ViewFile = dftracer::utils::trace::views::ViewFile;

#ifndef READS_PROJECTION_PLUGIN_PATH
#error "READS_PROJECTION_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

constexpr int NUM_EVENTS = 20;

// Every event carries args.x=i, so its sum across the trace is known; four
// more events each add one more distinct arg key (y, z, w, v), so an
// undeclared plugin's frame has five arg columns, not one.
ViewFile make_trace(dftu_utils_test::TestEnvironment& env) {
    const dstd::String pfw = env.dir() + "/reads_projection.pfw";
    dstd::String ofs;
    for (int i = 0; i < NUM_EVENTS; ++i) {
        ofs += common::str_cat(
            R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)",
            (1000 + i * 100), R"(,"dur":)", (10 + i), R"(,"args":{"x":)", i);
        if (i == 0) ofs += R"(,"y":1)";
        if (i == 1) ofs += R"(,"z":1)";
        if (i == 2) ofs += R"(,"w":1)";
        if (i == 3) ofs += R"(,"v":1)";
        ofs += "}}\n";
    }
    dstd::fs::write_file(dstd::PathView(pfw), ofs).discard();

    const dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dstd::fs::remove(pfw).discard();

    return test_plugin_common::index_trace(gz);
}

plugins::ConfigTree config_of(const dstd::String& json) {
    auto tree = plugins::ConfigTree::from_json_string(json);
    REQUIRE(tree.has_value());
    return std::move(*tree);
}

}  // namespace

TEST_CASE("a declared reads() narrows on_batch to exactly those columns") {
    auto set = plugins::Plugins::builder()
                   .add(READS_PROJECTION_PLUGIN_PATH,
                        config_of(R"({"declare_reads": true})"))
                   .build();
    INFO((set.has_value() ? dstd::String{}
                          : dstd::String(set.error().message())));
    REQUIRE(set.has_value());

    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    View view = View::from_files({make_trace(env)});
    plugins::PluginRun run =
        test_plugin_common::run_set(*set, view, "reads-projection");

    CHECK(test_plugin_common::result_text(run, "reads_probe.num_columns") ==
          "3");
    CHECK(test_plugin_common::result_text(run, "reads_probe.columns") ==
          "args.x,cat,dur");
    CHECK(test_plugin_common::result_text(run, "reads_probe.has_args_x") ==
          "1");
    CHECK(test_plugin_common::result_text(run, "reads_probe.args_x_sum") ==
          "190.000000");
}

TEST_CASE(
    "no declared reads() materializes every fixed column plus one per arg "
    "key") {
    auto set = plugins::Plugins::builder()
                   .add(READS_PROJECTION_PLUGIN_PATH,
                        config_of(R"({"declare_reads": false})"))
                   .build();
    INFO((set.has_value() ? dstd::String{}
                          : dstd::String(set.error().message())));
    REQUIRE(set.has_value());

    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    View view = View::from_files({make_trace(env)});
    plugins::PluginRun run =
        test_plugin_common::run_set(*set, view, "reads-projection");

    // 7 fixed columns (name, cat, pid, tid, ts, dur, ph) plus 5 distinct arg
    // keys (x, y, z, w, v): strictly more than the declared case's 3.
    CHECK(test_plugin_common::result_text(run, "reads_probe.num_columns") ==
          "12");
    CHECK(test_plugin_common::result_text(run, "reads_probe.columns") ==
          "args.v,args.w,args.x,args.y,args.z,cat,dur,name,ph,pid,tid,ts");
    CHECK(test_plugin_common::result_text(run, "reads_probe.has_args_x") ==
          "1");
    CHECK(test_plugin_common::result_text(run, "reads_probe.args_x_sum") ==
          "190.000000");
}

TEST_CASE("a path-decoded file hands the plugin its own columns") {
    auto set = plugins::Plugins::builder()
                   .add(READS_PROJECTION_PLUGIN_PATH,
                        config_of(R"({"declare_reads": false})"))
                   .build();
    REQUIRE(set.has_value());

    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    const dstd::String plain = env.dir() + "/generic.jsonl";
    {
        dstd::String ofs;
        for (int i = 0; i < NUM_EVENTS; ++i)
            ofs += common::str_cat(R"({"x":)", i, R"(,"k":"a"})", "\n");
        dstd::fs::write_file(dstd::PathView(plain), ofs).discard();
    }
    const dstd::String gz = plain + ".gz";
    dftu_utils_test::compress_file_to_gzip(plain, gz);
    dstd::fs::remove(plain).discard();
    dftu_utils_test::ok(
        dftu_utils_test::ok(dftracer::utils::index::Indexer::open(
                                {dstd::String(dstd::StringView(gz))}))
            .build());
    View view = View::from_files(
        {ViewFile{dstd::String(gz),
                  dstd::String(dstd::String(
                      trace::internal::determine_index_path(gz, "")))}});
    plugins::PluginRun run =
        test_plugin_common::run_set(*set, view, "reads-projection");

    CHECK(test_plugin_common::result_text(run, "reads_probe.columns") == "k,x");
    CHECK(test_plugin_common::result_text(run, "reads_probe.args_x_sum") ==
          "190.000000");

    // A projection of the View decodes only its paths for the scan; the
    // plugin still reads every field.
    auto narrowed = plugins::Plugins::builder()
                        .add(READS_PROJECTION_PLUGIN_PATH,
                             config_of(R"({"declare_reads": false})"))
                        .build();
    REQUIRE(narrowed.has_value());
    plugins::PluginRun projected = test_plugin_common::run_set(
        *narrowed,
        View::from_files(
            {ViewFile{dstd::String(gz),
                      dstd::String(dstd::String(
                          trace::internal::determine_index_path(gz, "")))}})
            .select({"k"}),
        "reads-projection");
    CHECK(test_plugin_common::result_text(
              projected, "reads_probe.args_x_sum") == "190.000000");
}
