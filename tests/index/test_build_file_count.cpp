#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

using namespace dftracer::utils;
namespace build = dftracer::utils::index::build;

namespace {

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
    return dftu_utils_test::write_gz_trace(
        dir + "/t" + std::to_string(n) + ".pfw.gz", out.str());
}

// RocksDB numbers every file it makes from one counter, so the highest SST
// number bounds the SSTs the build created, compacted or not.
std::uint64_t highest_sst_number(const std::string& index_path) {
    std::uint64_t highest = 0;
    for (const auto& entry : fs::directory_iterator(index_path)) {
        const auto name = entry.path().filename().string();
        if (entry.path().extension() != ".sst") continue;
        highest = std::max<std::uint64_t>(
            highest, std::stoull(name.substr(0, name.size() - 4)));
    }
    return highest;
}

}  // namespace

TEST_SUITE("IndexBuildFileCount") {
    TEST_CASE("small files that fit their memory share share SSTs") {
        constexpr int FILES = 48;
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < FILES; ++i)
            files.push_back(write_small_trace(env.get_dir(), i));
        const auto index_dir = env.get_dir() + "/idx";

        build::IndexBuildBatchResult result;
        Runtime rt(4);
        auto task = run_coro_scope(
            rt.executor(), [&](CoroScope& scope) -> coro::CoroTask<void> {
                auto config = std::make_shared<build::IndexBuildBatchConfig>();
                config->file_paths = files;
                config->index_dir = index_dir;
                config->parallelism = 4;
                result = co_await build::BatchBuilder::process(
                    &scope, std::move(config));
            });
        rt.submit(std::move(task), "file-count").wait();
        rt.shutdown();

        REQUIRE(result.indexed == FILES);
        const auto index_path =
            trace::internal::determine_index_path(files[0], index_dir);
        const auto created = highest_sst_number(index_path);
        MESSAGE("SSTs created for " << FILES << " files: " << created);
        CHECK(created < FILES);
    }
}
