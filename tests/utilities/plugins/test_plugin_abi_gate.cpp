// A plugin built against a different DFTRACER_PLUGIN_ABI_VERSION must be
// refused with a clear error at load, never crash. build_injected_plugins()
// (used by the other plugin tests) skips dlopen and the ABI gate entirely, so
// this drives Plugins::Builder against a real .so stamped with a wrong
// version by tests/utilities/plugins/bad_abi_version_plugin.c.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugins.h>
#include <doctest/doctest.h>

#include <string>

using dftracer::utils::plugins::Plugins;

#ifndef BAD_ABI_VERSION_PLUGIN_PATH
#error "BAD_ABI_VERSION_PLUGIN_PATH must be defined by CMake"
#endif

TEST_CASE("a plugin stamped with a mismatched ABI version is refused") {
    auto set = Plugins::builder().add(BAD_ABI_VERSION_PLUGIN_PATH).build();

    REQUIRE_FALSE(set.has_value());
    CHECK(set.error().message.find("ABI version") != std::string::npos);
    CHECK(set.error().message.find(BAD_ABI_VERSION_PLUGIN_PATH) !=
          std::string::npos);
}
