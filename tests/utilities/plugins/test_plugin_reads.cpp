// dftu_plugin::reads projects the batch down to the columns a plugin declares.
// Declaring none still materializes every fixed column plus one Series per
// distinct arg key in the batch - the cost this feature exists to avoid.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/plugins/config.h>
#include <dftracer/utils/plugins/plugins.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "test_plugin_common.h"

using dftracer::utils::plugins::ConfigTree;
using dftracer::utils::plugins::PluginRun;
using dftracer::utils::plugins::Plugins;
using View = dftracer::utils::trace::views::View;
using ViewFile = dftracer::utils::trace::views::ViewFile;
using test_plugin_common::index_trace;
using test_plugin_common::result_text;
using test_plugin_common::run_set;
namespace fs = std::filesystem;

#ifndef READS_PROJECTION_PLUGIN_PATH
#error "READS_PROJECTION_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

constexpr int NUM_EVENTS = 20;

// Every event carries args.x=i, so its sum across the trace is known; four
// more events each add one more distinct arg key (y, z, w, v), so an
// undeclared plugin's frame has five arg columns, not one.
ViewFile make_trace(dftu_utils_test::TestEnvironment& env) {
    const std::string pfw = env.get_dir() + "/reads_projection.pfw";
    std::ofstream ofs(pfw);
    for (int i = 0; i < NUM_EVENTS; ++i) {
        ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
            << (1000 + i * 100) << R"(,"dur":)" << (10 + i)
            << R"(,"args":{"x":)" << i;
        if (i == 0) ofs << R"(,"y":1)";
        if (i == 1) ofs << R"(,"z":1)";
        if (i == 2) ofs << R"(,"w":1)";
        if (i == 3) ofs << R"(,"v":1)";
        ofs << "}}\n";
    }
    ofs.close();

    const std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    fs::remove(pfw);

    return index_trace(gz);
}

ConfigTree config_of(const std::string& json) {
    return ConfigTree::from_json_string(json);
}

}  // namespace

TEST_CASE("a declared reads() narrows on_batch to exactly those columns") {
    auto set = Plugins::builder()
                   .add(READS_PROJECTION_PLUGIN_PATH,
                        config_of(R"({"declare_reads": true})"))
                   .build();
    INFO((set.has_value() ? std::string{} : set.error().message));
    REQUIRE(set.has_value());

    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    View view = View::from_files({make_trace(env)});
    PluginRun run = run_set(*set, view, "reads-projection");

    CHECK(result_text(run, "reads_probe.num_columns") == "3");
    CHECK(result_text(run, "reads_probe.columns") == "args.x,cat,dur");
    CHECK(result_text(run, "reads_probe.has_args_x") == "1");
    CHECK(result_text(run, "reads_probe.args_x_sum") == "190.000000");
}

TEST_CASE(
    "no declared reads() materializes every fixed column plus one per arg "
    "key") {
    auto set = Plugins::builder()
                   .add(READS_PROJECTION_PLUGIN_PATH,
                        config_of(R"({"declare_reads": false})"))
                   .build();
    INFO((set.has_value() ? std::string{} : set.error().message));
    REQUIRE(set.has_value());

    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    View view = View::from_files({make_trace(env)});
    PluginRun run = run_set(*set, view, "reads-projection");

    // 7 fixed columns (name, cat, pid, tid, ts, dur, ph) plus 5 distinct arg
    // keys (x, y, z, w, v): strictly more than the declared case's 3.
    CHECK(result_text(run, "reads_probe.num_columns") == "12");
    CHECK(result_text(run, "reads_probe.columns") ==
          "args.v,args.w,args.x,args.y,args.z,cat,dur,name,ph,pid,tid,ts");
    CHECK(result_text(run, "reads_probe.has_args_x") == "1");
    CHECK(result_text(run, "reads_probe.args_x_sum") == "190.000000");
}
