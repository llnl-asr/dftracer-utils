// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/coro.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/utilities/reader/internal/member_decode_cache.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>
#include <sys/wait.h>
#include <testing_runtime.h>
#include <testing_utilities.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace dftracer::utils;
namespace mdc = dftracer::utils::utilities::reader::internal;
using dftracer::utils::utilities::reader::ReadConfig;
using dftracer::utils::utilities::reader::TraceReader;
using dftracer::utils::utilities::reader::TraceReaderConfig;
using dftu_utils_test::run_coro;

namespace {

constexpr const char* TEST_DIR_ENV = "DFTRACER_UTILS_MEMBER_CACHE_EXIT_DIR";
constexpr const char* CHILD_CASE =
    "member cache: fills from a read then returns";

std::string trace_text(int n) {
    std::string text = "[\n";
    for (int i = 0; i < n; ++i) {
        text += R"({"id":)" + std::to_string(i) +
                R"(,"name":"op","cat":"POSIX","ts":)" +
                std::to_string(1000 + i) +
                R"(,"dur":10,"ph":"X"})"
                "\n";
    }
    return text;
}

}  // namespace

TEST_CASE("member cache: fills from a read then returns" * doctest::skip()) {
    fresh_process::announce();
    const char* dir = std::getenv(TEST_DIR_ENV);
    REQUIRE(dir != nullptr);
    const std::string root(dir);
    const std::string plain = root + "/plain.pfw";
    const std::string gz = root + "/trace.pfw.gz";
    const std::string index_dir = root + "/idx";
    {
        std::ofstream out(plain.c_str(), std::ios::binary);
        out << trace_text(4000);
    }
    REQUIRE(
        dftu_utils_test::compress_file_to_gzip_multimember(plain, gz, 4096));
    REQUIRE(dftu_utils_test::build_index(gz, index_dir));

    mdc::configure_global_member_decode_cache(64ull * 1024 * 1024);
    auto* cache = mdc::global_member_decode_cache();
    REQUIRE(cache != nullptr);

    std::int64_t lines = 0;
    run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
        TraceReader reader(
            TraceReaderConfig{dstd::String(gz), dstd::String(index_dir)});
        auto gen = reader.read_lines(ReadConfig{});
        while (co_await gen.next()) ++lines;
        co_return;
    });
    CHECK(lines >= 4000);
    CHECK(cache->stats().decodes > 0);
}

TEST_CASE("member cache: a process that filled the cache exits clean") {
    dftu_utils_test::ScopedTestDir dir("member_cache_exit");
    ::setenv(TEST_DIR_ENV, dir.str().c_str(), 1);
    ::setenv("DFTRACER_UTILS_MEMORY_EXIT_CHECK", "1", 1);

    const auto result = fresh_process::run(CHILD_CASE);

    ::unsetenv("DFTRACER_UTILS_MEMORY_EXIT_CHECK");
    ::unsetenv(TEST_DIR_ENV);

    REQUIRE(result.announced);
    CHECK(result.output.find("exit check failed") == std::string::npos);
    int status = result.status;
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}
