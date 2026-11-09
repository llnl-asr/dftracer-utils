// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/memory/detail/heap.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

using namespace dftracer::utils;

namespace {

constexpr int FILES = 8;
constexpr int EVENTS = 4000;
constexpr double MAX_PER_EVENT = 8.0;
constexpr double MAX_PER_FILE = 3900.0;

dstd::String write_trace(const dstd::String& dir, int n, int events = EVENTS) {
    dstd::String out;
    out += "[\n";
    for (int i = 0; i < events; ++i)
        out += common::str_cat(
            R"({"id":)", i, R"(,"name":"read","cat":"POSIX","pid":)", 1 + i % 4,
            R"(,"tid":1,"ph":"X","ts":)", 1000 + i * 10,
            R"(,"dur":3,"args":{"fname":"/data/file_number_)", n, "_", i % 37,
            R"(","size":)", i % 4096, R"(,"hhash":"a1b2c3d4e5f60718","level":)",
            i % 3, "}}\n");
    out += "]\n";
    return dftu_utils_test::write_gz_trace(
        dir + "/t" + dstd::String(dstd::to_string(n)) + ".pfw.gz", out);
}

}  // namespace

TEST_SUITE("IndexParseAllocations") {
    TEST_CASE("the index parse makes few global allocations per event") {
        dftu_utils_test::TestEnvironment env(10);
        dstd::Vector<dstd::String> files;
        for (int i = 0; i < FILES; ++i)
            files.emplace_back(write_trace(env.dir(), i));

        Runtime rt(4);
        index::build::IndexBuildBatchResult result;
        const auto pool_before = memory::detail::debug_allocation_count();
        auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
            auto config =
                dstd::make_shared<index::build::IndexBuildBatchConfig>();
            config->file_paths = files;
            config->index_dir = env.dir() + "/idx";
            config->parallelism = 4;
            result =
                co_await index::build::BatchBuilder::process(std::move(config));
        });
        dftu_utils_test::ok(
            rt.submit(std::move(task), "parse-allocations").wait());
        const auto pool =
            memory::detail::debug_allocation_count() - pool_before;
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
        dstd::Vector<dstd::String> files;
        for (int i = 0; i < SMALL_FILES; ++i)
            files.emplace_back(write_trace(env.dir(), i, SMALL_EVENTS));

        Runtime rt(4);
        index::build::IndexBuildBatchResult result;
        const auto pool_before = memory::detail::debug_allocation_count();
        auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
            auto config =
                dstd::make_shared<index::build::IndexBuildBatchConfig>();
            config->file_paths = files;
            config->index_dir = env.dir() + "/idx";
            config->parallelism = 4;
            result =
                co_await index::build::BatchBuilder::process(std::move(config));
        });
        dftu_utils_test::ok(
            rt.submit(std::move(task), "parse-allocations-small").wait());
        const auto pool =
            memory::detail::debug_allocation_count() - pool_before;
        REQUIRE(pool > 0);
        rt.shutdown();

        REQUIRE(result.indexed == SMALL_FILES);
        const double per_file = static_cast<double>(pool) / SMALL_FILES;
        MESSAGE("global allocations per file: " << per_file);
        CHECK(per_file < MAX_PER_FILE);
    }
}
