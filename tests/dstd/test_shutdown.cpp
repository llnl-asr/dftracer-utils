// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/shutdown.h>
#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <vector>

using dftracer::utils::dstd::add_shutdown_hook;
using dftracer::utils::dstd::shutdown;
using dftracer::utils::dstd::ShutdownStage;

namespace {

std::vector<std::string> g_ran;

void record(const char* what) { g_ran.emplace_back(what); }

}  // namespace

TEST_CASE("shutdown runs the stages in order and each stage in reverse") {
    add_shutdown_hook(ShutdownStage::CloseStores, [] { record("close 1"); });
    add_shutdown_hook(ShutdownStage::StopRuntime, [] { record("stop 1"); });
    add_shutdown_hook(ShutdownStage::FreeCaches, [] { record("free 1"); });
    add_shutdown_hook(ShutdownStage::CloseStores, [] { record("close 2"); });
    add_shutdown_hook(ShutdownStage::FreeCaches, [] { record("free 2"); });
    add_shutdown_hook(ShutdownStage::StopRuntime, [] { record("stop 2"); });
    add_shutdown_hook(ShutdownStage::FreeCaches, [] {
        record("nested");
        shutdown();
        record("nested done");
    });

    shutdown();

    const std::vector<std::string> want = {"close 2", "close 1", "stop 2",
                                           "stop 1",  "nested",  "nested done",
                                           "free 2",  "free 1"};
    CHECK(g_ran == want);
}

TEST_CASE("a second shutdown runs nothing") {
    const std::vector<std::string> after_first = g_ran;

    shutdown();

    CHECK(g_ran == after_first);
}
