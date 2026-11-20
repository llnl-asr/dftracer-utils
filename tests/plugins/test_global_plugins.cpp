// SPDX-License-Identifier: MIT
// The process-wide plugin set: a path loads once by its canonical form, every
// load must be matched by an unload, and a plugin cannot be unloaded while a
// scan through one of its formats is open.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/internal/file_format_registry.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/host/global_plugins.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <cstdio>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins::host;

#ifndef TOY_FORMAT_PLUGIN_PATH
#error "TOY_FORMAT_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

bool toy_registered() {
    return dataframe::find_file_format("toy_format.rows") != nullptr;
}

dstd::String write_rows() {
    const dstd::String path = "global_plugins_rows.toy";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    const char text[] = "TOY1\n1 alpha\n2 beta\n";
    REQUIRE(std::fwrite(text, 1, sizeof(text) - 1, f) == sizeof(text) - 1);
    std::fclose(f);
    return path;
}

}  // namespace

TEST_CASE("a path loaded twice stays until the last unload") {
    REQUIRE(load_plugin(TOY_FORMAT_PLUGIN_PATH).has_value());
    CHECK(toy_registered());
    REQUIRE(load_plugin(TOY_FORMAT_PLUGIN_PATH).has_value());
    REQUIRE(unload_plugin(TOY_FORMAT_PLUGIN_PATH).has_value());
    CHECK(toy_registered());
    REQUIRE(unload_plugin(TOY_FORMAT_PLUGIN_PATH).has_value());
    CHECK(!toy_registered());

    auto again = unload_plugin(TOY_FORMAT_PLUGIN_PATH);
    REQUIRE(!again.has_value());
    CHECK(dstd::String(again.error().c_str()).find("is not loaded") !=
          dstd::String::npos);
}

TEST_CASE("a plugin cannot be unloaded while a scan of its format is open") {
    const dstd::String path = write_rows();
    REQUIRE(load_plugin(TOY_FORMAT_PLUGIN_PATH).has_value());
    {
        dataframe::LazyFrame plan = dataframe::LazyFrame::scan_file(path);
        REQUIRE(plan.status().has_value());
        auto refused = unload_plugin(TOY_FORMAT_PLUGIN_PATH);
        REQUIRE(!refused.has_value());
        CHECK(dstd::String(refused.error().c_str()).find("still open") !=
              dstd::String::npos);
        CHECK(toy_registered());
        CHECK(
            dftu_utils_test::ok(default_runtime().submit(plan.collect()).get())
                .num_rows() == 2);
    }
    REQUIRE(unload_plugin(TOY_FORMAT_PLUGIN_PATH).has_value());
    CHECK(!toy_registered());
    std::remove(path.c_str());
}

TEST_CASE("a path that does not load is an error that names it") {
    auto loaded = load_plugin("/no/such/plugin.so");
    REQUIRE(!loaded.has_value());
    CHECK(dstd::String(loaded.error().c_str()).find("/no/such/plugin.so") !=
          dstd::String::npos);
}
