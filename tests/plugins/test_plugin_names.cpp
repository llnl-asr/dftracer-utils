// SPDX-License-Identifier: MIT
// The plugin namespace gate: a provider, file format or sink name belongs to
// the plugin that declares it, as its bare name or `<plugin>.<name>`.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/plugins/host/build_host.h>
#include <dftracer/utils/plugins/host/reserved_names.h>
#include <doctest/doctest.h>

using namespace dftracer::utils;

TEST_CASE("the plugin's own name belongs to it") {
    CHECK(plugins::host::name_in_plugin_namespace("toy", "toy"));
}

TEST_CASE("a dotted name under the plugin's own name belongs to it") {
    CHECK(plugins::host::name_in_plugin_namespace("toy.rows", "toy"));
    CHECK(plugins::host::name_in_plugin_namespace("toy.a.b", "toy"));
}

TEST_CASE("a bare name the plugin does not own is refused") {
    CHECK_FALSE(plugins::host::name_in_plugin_namespace("rows", "toy"));
}

TEST_CASE("a name that only starts with the plugin's name is refused") {
    CHECK_FALSE(plugins::host::name_in_plugin_namespace("toyz", "toy"));
    CHECK_FALSE(plugins::host::name_in_plugin_namespace("toyz.rows", "toy"));
}

TEST_CASE("a dotted name with nothing after the dot is refused") {
    CHECK_FALSE(plugins::host::name_in_plugin_namespace("toy.", "toy"));
}

TEST_CASE("another plugin's name is refused") {
    CHECK_FALSE(plugins::host::name_in_plugin_namespace("other.rows", "toy"));
    CHECK_FALSE(plugins::host::name_in_plugin_namespace("other", "toy"));
}

TEST_CASE("the host's dftu. prefix stays refused") {
    CHECK_FALSE(plugins::host::name_in_plugin_namespace("dftu.rows", "toy"));
    CHECK_FALSE(plugins::host::name_in_plugin_namespace("dftu.rows", "dftu"));
}

TEST_CASE("an absent name or plugin is refused") {
    CHECK_FALSE(plugins::host::name_in_plugin_namespace(
        static_cast<const char*>(nullptr), "toy"));
    CHECK_FALSE(plugins::host::name_in_plugin_namespace(
        "toy", static_cast<const char*>(nullptr)));
    CHECK_FALSE(plugins::host::name_in_plugin_namespace("", "toy"));
    CHECK_FALSE(plugins::host::name_in_plugin_namespace("toy", ""));
}

TEST_CASE("the op gate is unchanged: a plugin op needs a dotted name") {
    CHECK(plugins::host::refuse_plugin_op_name("toy"));
    CHECK(plugins::host::refuse_plugin_op_name("dftu.op"));
    CHECK_FALSE(plugins::host::refuse_plugin_op_name("toy.op"));
}

TEST_CASE("the build host gates on the declared name, the stem otherwise") {
    plugins::host::BuildHost host(dstd::String("libtoy"));
    CHECK(host.declared_name() == "libtoy");
    host.set_declared_name(dstd::String("toy"));
    CHECK(host.declared_name() == "toy");
    host.set_declared_name(dstd::String(""));
    CHECK(host.declared_name() == "libtoy");
}
