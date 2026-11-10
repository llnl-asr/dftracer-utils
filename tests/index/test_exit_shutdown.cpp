// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/index/cache/cache_store.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>
#include <sys/wait.h>
#include <testing_utilities.h>

#include <cstdlib>

using namespace dftracer::utils;
using namespace dftracer::utils::index;

namespace {

constexpr const char* TEST_DIR_ENV = "DFTRACER_UTILS_EXIT_SHUTDOWN_DIR";
constexpr const char* CHILD_CASE =
    "index: writes an index and a cache then returns";

dstd::String trace_text() {
    dstd::String text = "[\n";
    for (int i = 0; i < 8; ++i) {
        text +=
            R"({"id":)" + dstd::to_string(i + 1) +
            R"(,"name":"read","cat":"POSIX","pid":1,"tid":1,"ph":"X","ts":)" +
            dstd::to_string(1000 + i * 100) +
            R"(,"dur":10,"args":{"ret":0,"fhash":"a1b2c3d4e5f60718"}})"
            "\n";
    }
    text += "]\n";
    return text;
}

}  // namespace

TEST_CASE("index: writes an index and a cache then returns" * doctest::skip()) {
    fresh_process::announce();
    const char* dir = std::getenv(TEST_DIR_ENV);
    REQUIRE(dir != nullptr);
    const dstd::String root(dir);
    const dstd::String trace =
        dftu_utils_test::write_gz_trace(root + "/t.pfw.gz", trace_text());
    REQUIRE(!trace.empty());
    REQUIRE(dftu_utils_test::build_index(trace, root + "/.dftindex"));
    auto held = cache::CacheStore::open(root + "/cache",
                                        cache::CacheStore::Mode::ReadWrite);
    REQUIRE(held != nullptr);
    REQUIRE(held->put(7, {{"k", "v"}}, 1).has_value());
}

TEST_CASE("index: a process that wrote an index and a cache exits clean") {
    dftu_utils_test::ScopedTestDir dir("exit_shutdown");
    ::setenv(TEST_DIR_ENV, dir.str().c_str(), 1);
    ::setenv("DFTRACER_UTILS_MEMORY_EXIT_CHECK", "1", 1);

    const auto result = fresh_process::run(CHILD_CASE);

    ::unsetenv("DFTRACER_UTILS_MEMORY_EXIT_CHECK");
    ::unsetenv(TEST_DIR_ENV);

    REQUIRE(result.announced);
    CHECK(result.output.find("exit check failed") == dstd::String::npos);
    int status = result.status;
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}
