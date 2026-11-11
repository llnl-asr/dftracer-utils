// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>

#include <cstddef>

using namespace dftracer::utils;

namespace {

dstd::Vector<dstd::String> g_ran;

void record(const char* what) { g_ran.emplace_back(what); }

}  // namespace

TEST_CASE("shutdown runs the stages in order and each stage in reverse") {
    dstd::add_shutdown_hook(dstd::ShutdownStage::CloseStores,
                            []() noexcept { record("close 1"); });
    dstd::add_shutdown_hook(dstd::ShutdownStage::StopRuntime,
                            []() noexcept { record("stop 1"); });
    dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches,
                            []() noexcept { record("free 1"); });
    dstd::add_shutdown_hook(dstd::ShutdownStage::CloseStores,
                            []() noexcept { record("close 2"); });
    dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches,
                            []() noexcept { record("free 2"); });
    dstd::add_shutdown_hook(dstd::ShutdownStage::StopRuntime,
                            []() noexcept { record("stop 2"); });
    dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches, []() noexcept {
        record("nested");
        dstd::shutdown();
        record("nested done");
    });

    dstd::shutdown();

    const dstd::Vector<dstd::String> want = {
        "close 2", "close 1",     "stop 2", "stop 1",
        "nested",  "nested done", "free 2", "free 1"};
    CHECK(g_ran == want);
}

TEST_CASE("a second shutdown runs nothing") {
    const dstd::Vector<dstd::String> after_first = g_ran;

    dstd::shutdown();

    CHECK(g_ran == after_first);
}
