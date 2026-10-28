#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

using namespace dftracer::utils;
namespace build = dftracer::utils::index::build;
namespace store = dftracer::utils::index::store;

namespace {

constexpr int FILES = 64;
constexpr std::size_t WORKERS = 8;

std::string write_small_trace(const std::string& dir, int n) {
    std::ostringstream out;
    out << "[\n";
    for (int i = 0; i < 40; ++i) {
        out << R"({"id":)" << i << R"(,"name":"read","cat":"POSIX","pid":1,)"
            << R"("tid":1,"ph":"X","ts":)" << 1000 + i * 10
            << R"(,"dur":3,"args":{"fname":"/data/f)" << n << R"("}})"
            << "\n";
    }
    out << "]\n";
    char name[32];
    std::snprintf(name, sizeof(name), "/t%04d.pfw.gz", n);
    return dftu_utils_test::write_gz_trace(dir + name, out.str());
}

build::IndexBuildBatchResult build_with(const std::vector<std::string>& files,
                                        const std::string& index_dir,
                                        std::size_t workers) {
    build::IndexBuildBatchResult result;
    Runtime rt(workers);
    auto task = run_coro_scope(
        rt.executor(), [&](CoroScope& scope) -> coro::CoroTask<void> {
            auto config = std::make_shared<build::IndexBuildBatchConfig>();
            config->file_paths = files;
            config->index_dir = index_dir;
            config->parallelism = workers;
            result = co_await build::BatchBuilder::process(&scope,
                                                           std::move(config));
        });
    rt.submit(std::move(task), "build-parallelism").wait();
    rt.shutdown();
    return result;
}

}  // namespace

TEST_SUITE("IndexBuildParallelism") {
    TEST_CASE("an index build of many files keeps every worker busy") {
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < FILES; ++i)
            files.push_back(write_small_trace(env.get_dir(), i));

        const auto many =
            build_with(files, env.get_dir() + "/idx_many", WORKERS);
        const auto one = build_with(files, env.get_dir() + "/idx_one", 1);

        REQUIRE(many.indexed == FILES);
        REQUIRE(one.indexed == FILES);
        CHECK(many.metrics.max_files_in_flight >= WORKERS);
        CHECK(one.metrics.max_files_in_flight == 1);
        CHECK(many.total_events == one.total_events);

        auto sorted = [](std::vector<build::IndexBuildResult> r) {
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
