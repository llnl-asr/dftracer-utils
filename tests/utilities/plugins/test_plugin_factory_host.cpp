// SPDX-License-Identifier: MIT
// The factory is the plugin's init: it receives a host and may register with
// it. What it may do there is enforced, not documented - the build-phase host
// answers only the registration groups, and a factory that reaches past them
// fails the load instead of running on a host that is not there.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugins.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <utilities/plugins/test_plugin_common.h>

#include <cstdint>
#include <cstring>

using namespace dftracer::utils;

using View = dftracer::utils::trace::views::View;
using ViewFile = dftracer::utils::trace::views::ViewFile;

#ifndef FACTORY_REGISTERS_OP_PLUGIN_PATH
#error "FACTORY_REGISTERS_OP_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef FACTORY_ESCAPES_PLUGIN_PATH
#error "FACTORY_ESCAPES_PLUGIN_PATH must be defined by CMake"
#endif
#ifndef BUILDER_PLUGIN_PATH
#error "BUILDER_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

constexpr int EVENTS = 64;

ViewFile make_trace(dftu_utils_test::TestEnvironment& env,
                    const dstd::String& tag) {
    const dstd::String dir = env.dir() + "/" + tag;
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String pfw = dir + "/trace.pfw";
    dstd::String ofs;
    for (int i = 0; i < EVENTS; ++i)
        ofs += common::str_cat(
            R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)",
            (1000 + i * 100), R"(,"dur":)", (10 + i), R"(,"args":{}})", "\n");
    (void)dstd::fs::write_file(dstd::PathView(pfw), ofs);
    const dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    (void)dstd::fs::remove(pfw);

    return test_plugin_common::index_trace(gz);
}

}  // namespace

TEST_CASE("an op registered by the factory is live before the scan") {
    auto set = plugins::Plugins::builder()
                   .add(FACTORY_REGISTERS_OP_PLUGIN_PATH)
                   .build();
    INFO((set.has_value() ? dstd::String{}
                          : dstd::String(set.error().message())));
    REQUIRE(set.has_value());

    // Registration happened at load, so the op resolves before any batch has
    // been produced. That is the whole point of handing the factory a host.
    const dftu_op_desc* op = dftu_op_find("factory_registers_op.double_rows");
    REQUIRE(op != nullptr);

    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    View view = View::from_files({make_trace(env, "regop")});
    plugins::PluginRun run =
        test_plugin_common::run_set(*set, view, "factory-host");

    auto it = run.results.results().find("doubled_rows");
    REQUIRE(it != run.results.results().end());
    const auto& bytes = std::get<dstd::Vector<std::byte>>(it->second);
    REQUIRE(bytes.size() == sizeof(std::int64_t));
    std::int64_t total = 0;
    std::memcpy(&total, bytes.data(), sizeof(total));
    CHECK(total == 2 * EVENTS);
}

TEST_CASE("describe() lists the ops a plugin's factory registered") {
    auto set = plugins::Plugins::builder()
                   .add(FACTORY_REGISTERS_OP_PLUGIN_PATH)
                   .build();
    INFO((set.has_value() ? dstd::String{}
                          : dstd::String(set.error().message())));
    REQUIRE(set.has_value());

    auto info = set->describe();
    REQUIRE(info.size() == 1);
    // The signature comes from the registry, not from the plugin: an author
    // reading --describe needs to know how to call the op, not just that it
    // is there.
    REQUIRE(info[0].ops.size() == 1);
    // series in, scalar out, so the registry classes it as an aggregate.
    CHECK(info[0].ops[0] ==
          "factory_registers_op.double_rows(series) -> i64 [aggregate]");
}

TEST_CASE(
    "an op registered by a destroyed plugin is gone from the registry, and "
    "the plugin can be reloaded") {
    {
        auto set = plugins::Plugins::builder()
                       .add(FACTORY_REGISTERS_OP_PLUGIN_PATH)
                       .build();
        INFO((set.has_value() ? dstd::String{}
                              : dstd::String(set.error().message())));
        REQUIRE(set.has_value());
        REQUIRE(dftu_op_find("factory_registers_op.double_rows") != nullptr);
    }  // `set` destructs here: destroy() then dlclose unmaps the plugin.

    // A lookup after unload must come back NULL, not strcmp a name that lived
    // in the now-unmapped .so.
    CHECK(dftu_op_find("factory_registers_op.double_rows") == nullptr);

    // Reloading the same plugin must not be refused as an existing
    // registration (a stale entry would otherwise trip "no silent
    // shadowing").
    auto set2 = plugins::Plugins::builder()
                    .add(FACTORY_REGISTERS_OP_PLUGIN_PATH)
                    .build();
    INFO((set2.has_value() ? dstd::String{}
                           : dstd::String(set2.error().message())));
    REQUIRE(set2.has_value());
    CHECK(dftu_op_find("factory_registers_op.double_rows") != nullptr);
}

TEST_CASE("the scan-time host refuses to register an op") {
    auto set =
        plugins::Plugins::builder().add(SCAN_REGISTERS_OP_PLUGIN_PATH).build();
    INFO((set.has_value() ? dstd::String{}
                          : dstd::String(set.error().message())));
    REQUIRE(set.has_value());

    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    View view = View::from_files({make_trace(env, "scanreg")});
    plugins::PluginRun run =
        test_plugin_common::run_set(*set, view, "factory-host");

    // The plugin records whether register_op said no from on_batch.
    auto it = run.results.results().find("scan_register_refused");
    REQUIRE(it != run.results.results().end());
    const auto& bytes = std::get<dstd::Vector<std::byte>>(it->second);
    REQUIRE(bytes.size() == sizeof(std::int64_t));
    std::int64_t refused = 0;
    std::memcpy(&refused, bytes.data(), sizeof(refused));
    CHECK(refused == 1);

    // And nothing reached the registry, so no dangling entry survives the set.
    CHECK(dftu_op_find("scan_registers_op.late") == nullptr);
}

TEST_CASE("the C++ builder registers a fold, an op and a state at once") {
    auto set = plugins::Plugins::builder().add(BUILDER_PLUGIN_PATH).build();
    INFO((set.has_value() ? dstd::String{}
                          : dstd::String(set.error().message())));
    REQUIRE(set.has_value());

    CHECK(dftu_op_find("builder_plugin.double_rows") != nullptr);

    dftu_utils_test::TestEnvironment env(0);
    REQUIRE(env.is_valid());
    View view = View::from_files({make_trace(env, "builder")});
    plugins::PluginRun run =
        test_plugin_common::run_set(*set, view, "factory-host");

    // The fold half and the registered-state half of the same factory.
    CHECK(test_plugin_common::result_text(run, "builder_plugin.rows") ==
          dstd::to_string(EVENTS));
    std::uint64_t want_dur = 0;
    for (int i = 0; i < EVENTS; ++i)
        want_dur += static_cast<std::uint64_t>(10 + i);
    CHECK(test_plugin_common::result_text(run, "builder_plugin.total_dur") ==
          dstd::to_string(want_dur));
}

TEST_CASE("a factory reaching past the registration surface fails the load") {
    auto set =
        plugins::Plugins::builder().add(FACTORY_ESCAPES_PLUGIN_PATH).build();

    REQUIRE_FALSE(set.has_value());
    CHECK(set.error().message().find(DFTU_SVC_IO) != dstd::String::npos);
    CHECK(set.error().message().find("build-phase host") != dstd::String::npos);
    CHECK(set.error().message().find(FACTORY_ESCAPES_PLUGIN_PATH) !=
          dstd::String::npos);
}
