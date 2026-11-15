// SPDX-License-Identifier: MIT
// A body that registers a fold and a UDF and then returns an error loads
// nothing: the load fails with the body's message, the host rolls the UDF back,
// and the fold the builder still held is destroyed, so a process that stops
// right after the failed load passes the memory exit check.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/udf_registry.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>

#include <cstdlib>

using namespace dftracer::utils;

#ifndef ERROR_AFTER_REGISTER_PLUGIN_PATH
#error "ERROR_AFTER_REGISTER_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

// The config the fixture reads: a plan long enough to leave the small-string
// buffer, so the fold holder owns a block the exit check can see.
constexpr const char* CONFIG_JSON =
    R"({"duql": "select name, dur where dur > 1000 and name contains 'io'"})";

dstd::Result<plugins::host::Plugins> load() {
    return plugins::host::Plugins::builder()
        .add(ERROR_AFTER_REGISTER_PLUGIN_PATH, dstd::String(CONFIG_JSON))
        .build();
}

}  // namespace

TEST_CASE("a body that registers a fold and a udf then fails the load") {
    auto set = load();
    INFO((set.has_value() ? dstd::String{}
                          : dstd::String(set.error().message())));
    REQUIRE_FALSE(set.has_value());
    CHECK(set.error().condition() == dstd::Condition::InvalidArgument);
    CHECK(set.error().message().find("refusing to load") != dstd::String::npos);
    CHECK(dataframe::find_udf("refused") == nullptr);
}

// Run only from the case below, in a process that starts with no state from
// this one and exits with the memory exit check on.
TEST_CASE("failed load at exit" * doctest::skip()) {
    fresh_process::announce();
    auto set = load();
    REQUIRE_FALSE(set.has_value());
}

TEST_CASE("the fold a failed load registered leaves no memory at exit") {
    ::setenv("DFTRACER_UTILS_MEMORY_EXIT_CHECK", "1", 1);
    auto result = fresh_process::run("failed load at exit");
    ::unsetenv("DFTRACER_UTILS_MEMORY_EXIT_CHECK");
    REQUIRE(result.announced);
    CHECK(result.output.find("exit check failed") ==
          decltype(result.output)::npos);
    CHECK(result.ok());
}
