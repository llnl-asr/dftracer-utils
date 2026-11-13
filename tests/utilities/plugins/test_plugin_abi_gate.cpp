// SPDX-License-Identifier: MIT
// A plugin built against a different plugin ABI must be refused with a clear
// error at load, never crash. build_injected_plugins() (used by the other
// plugin tests) skips dlopen and the ABI gate entirely, so this drives
// Plugins::Builder against real .so files: one stamped with a wrong
// DFTRACER_UTILS_PLUGIN_ABI_VERSION, one with a wrong header hash, one with no
// dftracer_plugin_info symbol (tests/utilities/plugins/bad_abi_*.c,
// no_info_plugin.c). Each plugin's factory fails with a distinct message if
// the loader runs it, so a refusal that names the ABI proves the gate ran
// first.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <doctest/doctest.h>

using namespace dftracer::utils;

#ifndef BAD_ABI_VERSION_PLUGIN_PATH
#error "BAD_ABI_VERSION_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef BAD_ABI_HASH_PLUGIN_PATH
#error "BAD_ABI_HASH_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef NO_INFO_PLUGIN_PATH
#error "NO_INFO_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

bool contains(const dstd::String& text, const char* part) {
    return text.find(part) != dstd::String::npos;
}

}  // namespace

TEST_CASE("a plugin stamped with a mismatched ABI version is refused") {
    auto set = plugins::host::Plugins::builder()
                   .add(BAD_ABI_VERSION_PLUGIN_PATH)
                   .build();

    REQUIRE_FALSE(set.has_value());
    const dstd::String message(set.error().message());
    CHECK(contains(message, "other plugin ABI headers"));
    CHECK(contains(message, BAD_ABI_VERSION_PLUGIN_PATH));
    CHECK_FALSE(contains(message, "the loader must not get here"));
}

TEST_CASE("a plugin with a mismatched ABI header hash is refused") {
    auto set =
        plugins::host::Plugins::builder().add(BAD_ABI_HASH_PLUGIN_PATH).build();

    REQUIRE_FALSE(set.has_value());
    const dstd::String message(set.error().message());
    CHECK(contains(message, "other plugin ABI headers"));
    CHECK(contains(message, "hash"));
    CHECK(contains(message, BAD_ABI_HASH_PLUGIN_PATH));
    CHECK_FALSE(contains(message, "the loader must not get here"));
}

TEST_CASE("a plugin with no info symbol is refused") {
    auto set =
        plugins::host::Plugins::builder().add(NO_INFO_PLUGIN_PATH).build();

    REQUIRE_FALSE(set.has_value());
    const dstd::String message(set.error().message());
    CHECK(contains(message, "dftracer_plugin_info"));
    CHECK(contains(message, NO_INFO_PLUGIN_PATH));
    CHECK_FALSE(contains(message, "the loader must not get here"));
}
