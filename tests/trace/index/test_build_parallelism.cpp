// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/build/batch_builder.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>

using namespace dftracer::utils;

namespace {

constexpr int FILES = 64;
constexpr std::size_t WORKERS = 8;

dstd::String write_small_trace(dstd::CStringView dir, int n) {
    dstd::String out;
    out += "[\n";
    for (int i = 0; i < 40; ++i) {
        dstd::fmt_append<
            "{\"id\":{},\"name\":\"read\",\"cat\":\"POSIX\",\"pid\":1,\"tid\":"
            "1,\"ph\":1,\"ts\":{},\"dur\":3,\"args\":{\"fname\":\"/data/"
            "f{}\"}}\n">(out, i, 1000 + i * 10, n);
    }
    out += "]\n";
    return dftu_utils_test::write_gz_trace(
        dstd::String(dir) + dstd::fmt<"/t{:04}.pfw.gz">(n), out);
}

trace::index::build::IndexBuildBatchResult build_with(
    dstd::Span<const dstd::String> files, dstd::CStringView index_dir,
    std::size_t workers) {
    trace::index::build::IndexBuildBatchResult result;
    Runtime rt(workers);
    auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
        auto config =
            dstd::make_shared<trace::index::build::IndexBuildBatchConfig>();
        config->file_paths.assign(files.begin(), files.end());
        config->index_dir = index_dir;
        config->parallelism = workers;
        result = co_await trace::index::build::BatchBuilder::process(
            std::move(config));
    });
    dftu_utils_test::ok(rt.submit(std::move(task), "build-parallelism").wait());
    rt.shutdown();
    return result;
}

}  // namespace

TEST_SUITE("IndexBuildParallelism") {
    TEST_CASE("an index build of many files keeps every worker busy") {
        dftu_utils_test::TestEnvironment env(10);
        dstd::Vector<dstd::String> files;
        for (int i = 0; i < FILES; ++i)
            files.emplace_back(write_small_trace(env.dir(), i));

        const auto many = build_with(files, env.dir() + "/idx_many", WORKERS);
        const auto one = build_with(files, env.dir() + "/idx_one", 1);

        REQUIRE(many.indexed == FILES);
        REQUIRE(one.indexed == FILES);
        CHECK(many.metrics.max_files_in_flight >= WORKERS);
        CHECK(one.metrics.max_files_in_flight == 1);
        CHECK(many.total_events == one.total_events);

        auto sorted =
            [](dstd::Vector<trace::index::build::IndexBuildResult> r) {
                std::sort(r.begin(), r.end(), [](const auto& a, const auto& b) {
                    return a.file_path < b.file_path;
                });
                return r;
            };
        const auto a = sorted(many.results);
        const auto b = sorted(one.results);
        REQUIRE(a.size() == b.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            CHECK(a[i].file_path == b[i].file_path);
            CHECK(a[i].total_lines == b[i].total_lines);
            CHECK(a[i].chunks_processed == b[i].chunks_processed);
            CHECK(a[i].events_processed == b[i].events_processed);
        }
    }
}
