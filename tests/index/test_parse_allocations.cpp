// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <sstream>
#include <string>
#include <vector>

using namespace dftracer::utils;
namespace build = dftracer::utils::index::build;

namespace {

constexpr int FILES = 8;
constexpr int EVENTS = 4000;
constexpr double MAX_PER_EVENT = 8.0;
constexpr double MAX_PER_FILE = 3900.0;

std::string write_trace(const std::string& dir, int n, int events = EVENTS) {
    std::ostringstream out;
    out << "[\n";
    for (int i = 0; i < events; ++i)
        out << R"({"id":)" << i << R"(,"name":"read","cat":"POSIX","pid":)"
            << 1 + i % 4 << R"(,"tid":1,"ph":"X","ts":)" << 1000 + i * 10
            << R"(,"dur":3,"args":{"fname":"/data/file_number_)" << n << "_"
            << i % 37 << R"(","size":)" << i % 4096
            << R"(,"hhash":"a1b2c3d4e5f60718","level":)" << i % 3 << "}}\n";
    out << "]\n";
    return dftu_utils_test::write_gz_trace(
        dir + "/t" + std::to_string(n) + ".pfw.gz", out.str());
}

}  // namespace

TEST_SUITE("IndexParseAllocations") {
    TEST_CASE("the index parse makes few global allocations per event") {
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < FILES; ++i)
            files.push_back(write_trace(env.get_dir(), i));

        Runtime rt(4);
        build::IndexBuildBatchResult result;
        const auto pool_before = memory::debug_allocation_count();
        auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
            auto config = std::make_shared<build::IndexBuildBatchConfig>();
            config->file_paths = files;
            config->index_dir = env.get_dir() + "/idx";
            config->parallelism = 4;
            result = co_await build::BatchBuilder::process(std::move(config));
        });
        rt.submit(std::move(task), "parse-allocations").wait();
        const auto pool = memory::debug_allocation_count() - pool_before;
        REQUIRE(pool > 0);
        rt.shutdown();

        REQUIRE(result.indexed == FILES);
        const double per_event = static_cast<double>(pool) / (FILES * EVENTS);
        MESSAGE("global allocations per event: " << per_event);
        CHECK(per_event < MAX_PER_EVENT);
    }

    TEST_CASE(
        "a fresh build of many small files makes few allocations "
        "per file") {
        // Small files make the per-file work count. A fresh file id has
        // nothing to clear, so it must allocate nothing for clears.
        constexpr int SMALL_FILES = 64;
        constexpr int SMALL_EVENTS = 500;
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < SMALL_FILES; ++i)
            files.push_back(write_trace(env.get_dir(), i, SMALL_EVENTS));

        Runtime rt(4);
        build::IndexBuildBatchResult result;
        const auto pool_before = memory::debug_allocation_count();
        auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
            auto config = std::make_shared<build::IndexBuildBatchConfig>();
            config->file_paths = files;
            config->index_dir = env.get_dir() + "/idx";
            config->parallelism = 4;
            result = co_await build::BatchBuilder::process(std::move(config));
        });
        rt.submit(std::move(task), "parse-allocations-small").wait();
        const auto pool = memory::debug_allocation_count() - pool_before;
        REQUIRE(pool > 0);
        rt.shutdown();

        REQUIRE(result.indexed == SMALL_FILES);
        const double per_file = static_cast<double>(pool) / SMALL_FILES;
        MESSAGE("global allocations per file: " << per_file);
        CHECK(per_file < MAX_PER_FILE);
    }
}
