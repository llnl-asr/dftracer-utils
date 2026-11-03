// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/build/index_fold_driver.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <cstdint>

using namespace dftracer::utils;
namespace build = dftracer::utils::index::build;

namespace {

dstd::String write_small_trace(const dstd::String& dir, int n) {
    dstd::String out;
    out += "[\n";
    for (int i = 0; i < 40; ++i) {
        out += common::str_cat(
            R"({"id":)", i, R"(,"name":"read","cat":"POSIX","pid":1,)",
            R"("tid":1,"ph":"X","ts":)", 1000 + i * 10,
            R"(,"dur":3,"args":{"fname":"/data/f)", n, R"("}})", "\n");
    }
    out += "]\n";
    return dftu_utils_test::write_gz_trace(
        dir + "/t" + dstd::String(dstd::to_string(n)) + ".pfw.gz", out);
}

dstd::String write_long_line_trace(const dstd::String& dir, std::size_t bytes) {
    dstd::String out;
    out += "[\n";
    for (int i = 0; i < 40; ++i) {
        out += common::str_cat(R"({"id":)", i,
                               R"(,"name":"read","cat":"POSIX","pid":1,)",
                               R"("tid":1,"ph":"X","ts":)", 1000 + i * 10,
                               R"(,"dur":3,"args":{"fname":"/data/long")");
        if (i == 20)
            out +=
                common::str_cat(R"(,"note":")", dstd::String(bytes, 'x'), '"');
        out += "}}\n";
    }
    out += "]\n";
    return dftu_utils_test::write_gz_trace(dir + "/long.pfw.gz", out);
}

}  // namespace

TEST_SUITE("IndexParserReuse") {
    TEST_CASE("an index build parses every file with a worker's parser") {
        constexpr int FILES = 240;
        constexpr int WORKERS = 2;
        dftu_utils_test::TestEnvironment env(10);
        dstd::Vector<dstd::String> files;
        for (int i = 0; i < FILES; ++i)
            files.emplace_back(write_small_trace(env.get_dir(), i));
        constexpr std::size_t RETAIN =
            build::IndexFoldDriver::PARSER_RETAIN_BYTES;
        constexpr std::size_t LONG_LINE = 2 * RETAIN;
        files.emplace(files.begin() + FILES / 2,
                      write_long_line_trace(env.get_dir(), LONG_LINE));

        const auto created_before = build::g_parsers_created.load();
        build::IndexBuildBatchResult result;
        Runtime rt(WORKERS);
        auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
            auto config = dstd::make_shared<build::IndexBuildBatchConfig>();
            config->file_paths = files;
            config->index_dir = env.get_dir() + "/idx";
            config->parallelism = WORKERS;
            result = co_await build::BatchBuilder::process(std::move(config));
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
