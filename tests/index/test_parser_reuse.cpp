#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/build/index_fold_driver.h>
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

std::string write_long_line_trace(const std::string& dir, std::size_t bytes) {
    std::ostringstream out;
    out << "[\n";
    for (int i = 0; i < 40; ++i) {
        out << R"({"id":)" << i << R"(,"name":"read","cat":"POSIX","pid":1,)"
            << R"("tid":1,"ph":"X","ts":)" << 1000 + i * 10
            << R"(,"dur":3,"args":{"fname":"/data/long")";
        if (i == 20) out << R"(,"note":")" << std::string(bytes, 'x') << '"';
        out << "}}\n";
    }
    out << "]\n";
    return dftu_utils_test::write_gz_trace(dir + "/long.pfw.gz", out.str());
}

}  // namespace

TEST_SUITE("IndexParserReuse") {
    TEST_CASE("an index build parses every file with a worker's parser") {
        constexpr int FILES = 240;
        constexpr int WORKERS = 2;
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < FILES; ++i)
            files.push_back(write_small_trace(env.get_dir(), i));
        constexpr std::size_t RETAIN =
            build::IndexFoldDriver::PARSER_RETAIN_BYTES;
        constexpr std::size_t LONG_LINE = 2 * RETAIN;
        files.insert(files.begin() + FILES / 2,
                     write_long_line_trace(env.get_dir(), LONG_LINE));

        const auto created_before = build::g_parsers_created.load();
        build::IndexBuildBatchResult result;
        Runtime rt(WORKERS);
        auto task = run_coro_scope(
            rt.executor(), [&](CoroScope& scope) -> coro::CoroTask<void> {
                auto config = std::make_shared<build::IndexBuildBatchConfig>();
                config->file_paths = files;
                config->index_dir = env.get_dir() + "/idx";
                config->parallelism = WORKERS;
                result = co_await build::BatchBuilder::process(
                    &scope, std::move(config));
            });
        rt.submit(std::move(task), "parser-reuse").wait();

        REQUIRE(result.indexed == FILES + 1);
        const auto created = build::g_parsers_created.load() - created_before;
        const auto peak = build::g_parser_peak_capacity.load();
        const auto live = build::g_parser_live_capacity.load();
        MESSAGE("parsers created: " << created << ", peak capacity: " << peak
                                    << ", live capacity: " << live);
        rt.shutdown();
        CHECK(created <= 4 * WORKERS);
        CHECK(peak >= LONG_LINE);
        CHECK(peak <= 4 * WORKERS * RETAIN);
        CHECK(live <= WORKERS * RETAIN);
    }
}
