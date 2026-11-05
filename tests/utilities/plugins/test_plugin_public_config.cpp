// SPDX-License-Identifier: MIT
// Builder::add(path, ConfigTree) takes a type only reachable through the
// private src/ header, so a caller with only <dftracer/utils/plugins/
// plugins.h> could not build config for it. This file includes ONLY the
// public header (no plugins/config.h) to prove the JSON-string overload
// closes that gap: a config literal, loaded and its values checked through a
// real plugin run, and a malformed literal failing at build() rather than
// add().

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/plugins/plugins.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <utilities/plugins/test_plugin_common.h>

using View = dftracer::utils::trace::views::View;
using ViewFile = dftracer::utils::trace::views::ViewFile;

#ifndef CONFIG_KEYS_PLUGIN_PATH
#error "CONFIG_KEYS_PLUGIN_PATH must be defined by CMake"
#endif

using namespace dftracer::utils;
namespace {

constexpr int NUM_EVENTS = 5;

ViewFile make_trace(dftu_utils_test::TestEnvironment& env) {
    const dstd::String pfw = env.dir() + "/public_config.pfw";
    dstd::String ofs;
    for (int i = 0; i < NUM_EVENTS; ++i) {
        ofs += common::str_cat(
            R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)",
            (1000 + i * 100), R"(,"dur":10,"args":{}})", "\n");
    }
    (void)dstd::fs::write_file(dstd::PathView(pfw), ofs);

    const dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    (void)dstd::fs::remove(pfw);

    return test_plugin_common::index_trace(gz);
}

}  // namespace

TEST_CASE(
    "add(path, json) builds config from only the public header and the "
    "plugin receives it") {
    auto set = plugins::Plugins::builder()
                   .add(CONFIG_KEYS_PLUGIN_PATH,
                        dstd::String(R"({"label": "rows", "stride": 3})"))
                   .build();
    INFO((set.has_value() ? dstd::String{}
                          : dstd::String(set.error().message())));
    REQUIRE(set.has_value());

    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    View view = View::from_files({make_trace(env)});
    plugins::PluginRun run =
        test_plugin_common::run_set(*set, view, "public-config");

    // rows = NUM_EVENTS * stride; a wrong stride (the config default is 1)
    // would report NUM_EVENTS instead.
    CHECK(dstd::StringView(test_plugin_common::result_text(
              run, "config_keys_plugin.rows")) ==
          dstd::to_string(NUM_EVENTS * 3));
}

TEST_CASE("add(path, json) fails at build(), not at add()") {
    plugins::Plugins::Builder builder = plugins::Plugins::builder();
    REQUIRE_NOTHROW(
        builder.add(CONFIG_KEYS_PLUGIN_PATH, dstd::String("{not json")));

    auto set = builder.build();
    REQUIRE(!set.has_value());
    CHECK(set.error().message().find("does not parse") != dstd::String::npos);
}
