// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <sstream>

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
    const std::string gz =
        dir + "/t" + std::string(dstd::to_string(n)) + ".pfw.gz";
    return dftu_utils_test::write_gz_trace(gz, out.str());
}

}  // namespace

TEST_SUITE("IndexFoldDriver") {
    // A buffer a fold still held when its driver closed would be freed under
    // another driver's pool, leaving live bytes in one pool and a negative
    // count in another. The drivers of a build share one pool.
    TEST_CASE("fold buffer pools close with no live bytes over many files") {
        dftu_utils_test::TestEnvironment env(10);
        dstd::Vector<dstd::String> files;
        for (int i = 0; i < 256; ++i)
            files.emplace_back(write_trace(env.get_dir(), i, 300));

        build::IndexBuildBatchResult result;
        Runtime rt(8);
        auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
            auto config = dstd::make_shared<build::IndexBuildBatchConfig>();
            config->file_paths = files;
            config->index_dir = env.get_dir() + "/idx";
            config->parallelism = 8;
            result = co_await build::BatchBuilder::process(std::move(config));
        });
        rt.submit(std::move(task), "fold-buffer-pools").wait();
        rt.shutdown();

        REQUIRE(result.indexed == files.size());
        CHECK_FALSE(memory::Manager::get().leaked());
    }
}
