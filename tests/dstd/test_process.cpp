// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/process.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>

using namespace dftracer::utils;

TEST_CASE("process_is_alive tells a running process from a gone one") {
    CHECK(dstd::process_is_alive(static_cast<std::uint64_t>(::getpid())));
    CHECK(dstd::process_is_alive(static_cast<std::uint64_t>(::getppid())));

    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) ::_exit(0);
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    CHECK_FALSE(dstd::process_is_alive(static_cast<std::uint64_t>(child)));
}
