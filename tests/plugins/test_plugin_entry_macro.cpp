// SPDX-License-Identifier: MIT
// DFTU_PLUGIN is the whole entry point: it declares the info record the loader
// reads, registers what the body asks for, and fails the load with the error
// the body returned. Both fixtures below are written against the SDK alone.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <doctest/doctest.h>

#include <cstddef>

#ifndef MACRO_PLUGIN_PATH
#error "MACRO_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef MACRO_ERROR_PLUGIN_PATH
#error "MACRO_ERROR_PLUGIN_PATH must be defined by CMake"
#endif

using namespace dftracer::utils;

TEST_CASE("a plugin whose body only fills the builder loads") {
    auto set = plugins::host::Plugins::builder().add(MACRO_PLUGIN_PATH).build();
    INFO((set.has_value() ? dstd::String{}
                          : dstd::String(set.error().message())));
    REQUIRE(set.has_value());
    REQUIRE(set->size() == 1);

    auto info = set->describe();
    REQUIRE(info.size() == 1);
    CHECK(info[0].name == "macro_plugin");
    CHECK(info[0].version == "1.0");
    CHECK(info[0].has_fold);
}

TEST_CASE("an error returned by the body fails the load with its message") {
    auto set =
        plugins::host::Plugins::builder().add(MACRO_ERROR_PLUGIN_PATH).build();

    REQUIRE_FALSE(set.has_value());
    CHECK(set.error().condition() == dstd::Condition::InvalidArgument);
    CHECK(set.error().message().find("macro_plugin: refusing to load") !=
          dstd::String::npos);
}
