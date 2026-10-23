#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <sstream>
#include <string>
#include <vector>

using namespace dftracer::utils;
namespace build = dftracer::utils::index::build;

namespace {

std::string write_trace(const std::string& dir, int n, int events) {
    std::ostringstream out;
    out << "[\n";
    for (int i = 0; i < events; ++i)
        out << R"({"id":)" << i << R"(,"name":"read","cat":"POSIX","pid":)"
            << 1 + n % 7 << R"(,"tid":1,"ph":"X","ts":)" << 1000 + i * 10
            << R"(,"dur":3,"args":{"fname":"/data/f)" << n << "_" << i % 17
            << R"(","size":)" << i % 4096 << "}}\n";
    out << "]\n";
    const std::string gz = dir + "/t" + std::to_string(n) + ".pfw.gz";
    return dftu_utils_test::write_gz_trace(gz, out.str());
}

}  // namespace

TEST_SUITE("IndexFoldDriver") {
    // Each file's driver once had a "fold buffers" pool of its own; a buffer
    // a fold still held when its driver closed was freed under another
    // driver's pool, so one pool closed with 16 live bytes and another went
    // to -16 (seen on 8,404 files on Corona). The drivers of a build now
    // share one pool.
    TEST_CASE("fold buffer pools close with no live bytes over many files") {
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < 256; ++i)
            files.push_back(write_trace(env.get_dir(), i, 300));

        build::IndexBuildBatchResult result;
        Runtime rt(8);
        auto task = run_coro_scope(
            rt.executor(), [&](CoroScope& scope) -> coro::CoroTask<void> {
                auto config = std::make_shared<build::IndexBuildBatchConfig>();
                config->file_paths = files;
                config->index_dir = env.get_dir() + "/idx";
                config->parallelism = 8;
                result = co_await build::BatchBuilder::process(
                    &scope, std::move(config));
            });
        rt.submit(std::move(task), "fold-buffer-pools").wait();
        rt.shutdown();

        REQUIRE(result.indexed == files.size());
        CHECK_FALSE(memory::Manager::get().leaked());
    }
}
