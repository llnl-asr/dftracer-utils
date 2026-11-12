// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/runtime/io/io_backend.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/scheduler/task_executor.h>
#include <doctest/doctest.h>
#include <memory/fresh_process.h>

#include <cstdio>
#include <cstdlib>

using namespace dftracer::utils;

namespace {

constexpr const char* BACKEND_ENV = "DFTRACER_UTILS_IO_BACKEND";

fresh_process::Result run_with(const char* value, const char* test_case) {
    ::setenv(BACKEND_ENV, value, 1);
    auto result = fresh_process::run(test_case);
    ::unsetenv(BACKEND_ENV);
    return result;
}

}  // namespace

TEST_CASE("child: report the backend" * doctest::skip()) {
    fresh_process::announce();
    ExecutorConfig config;
    config.thread_count = 2;
    config.io_pool_size = 2;
    auto executor = make_task_executor(config);
    REQUIRE(executor->start().has_value());
    REQUIRE(executor->has_io_backend());
    std::fprintf(stderr, "backend=%s\n", executor->io_backend().name().c_str());
    executor->shutdown();
}

TEST_CASE("child: start with a bogus backend" * doctest::skip()) {
    fresh_process::announce();
    ExecutorConfig config;
    config.thread_count = 2;
    auto executor = make_task_executor(config);
    auto started = executor->start();
    REQUIRE_FALSE(started.has_value());
    std::fprintf(stderr, "start failed: %s\n", started.error().c_str());
    REQUIRE_FALSE(executor->has_io_backend());
}

TEST_CASE("child: a runtime with a bogus backend ends the process" *
          doctest::skip()) {
    fresh_process::announce();
    Runtime runtime(2);
    std::fprintf(stderr, "started\n");
}

TEST_CASE("the env var forces the thread pool") {
    auto result = run_with("threadpool", "child: report the backend");
    CHECK(result.ok());
    CHECK(result.output.find("backend=threadpool\n") != dstd::String::npos);
}

TEST_CASE("an unknown backend fails to start") {
    auto result =
        run_with("bogus_backend", "child: start with a bogus backend");
    CHECK(result.ok());
    CHECK(result.output.find("start failed: ") != dstd::String::npos);
    CHECK(result.output.find(BACKEND_ENV) != dstd::String::npos);
    CHECK(result.output.find("'bogus_backend'") != dstd::String::npos);
}

TEST_CASE("a runtime with a bogus backend ends the process") {
    auto result =
        run_with("bogus_backend",
                 "child: a runtime with a bogus backend ends the process");
    CHECK(result.aborted_with(BACKEND_ENV));
    CHECK(result.output.find("started\n") == dstd::String::npos);
}
