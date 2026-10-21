#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/scheduler/task_executor.h>
#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../memory/fresh_process.h"

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
    config.num_threads = 2;
    config.io_pool_size = 2;
    auto executor = make_task_executor(config);
    executor->start();
    REQUIRE(executor->has_io_backend());
    std::fprintf(stderr, "backend=%s\n", executor->io_backend().name().c_str());
    executor->shutdown();
}

TEST_CASE("child: start with a bogus backend" * doctest::skip()) {
    fresh_process::announce();
    try {
        Runtime runtime(2);
    } catch (const DFTUtilsException& e) {
        std::fprintf(stderr, "start failed: %s\n", e.what());
        return;
    }
    std::fprintf(stderr, "started\n");
    FAIL("the runtime started");
}

TEST_CASE("the env var forces the thread pool") {
    auto result = run_with("threadpool", "child: report the backend");
    CHECK(result.ok());
    CHECK(result.output.find("backend=threadpool\n") != std::string::npos);
}

TEST_CASE("an unknown backend fails to start") {
    auto result =
        run_with("bogus_backend", "child: start with a bogus backend");
    CHECK(result.ok());
    CHECK(result.output.find("start failed: ") != std::string::npos);
    CHECK(result.output.find("'bogus_backend'") != std::string::npos);
    CHECK(result.output.find("started\n") == std::string::npos);
}
