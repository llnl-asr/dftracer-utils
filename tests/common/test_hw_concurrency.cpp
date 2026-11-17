// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <doctest/doctest.h>

#if defined(__linux__)
#include <sched.h>

#include <cstdlib>

using namespace dftracer::utils;
#endif

TEST_CASE("the default worker count is never 0") {
    CHECK(dftracer::utils::common::hardware_concurrency() >= 1);
}

TEST_CASE(
    "a rank's thread budget divides the node only when the launcher did not") {
    using dftracer::utils::common::threads_per_local_rank;
    // Bound by the launcher: the mask is already this rank's share.
    CHECK(threads_per_local_rank(1, 48, 8) == 1);
    CHECK(threads_per_local_rank(4, 48, 8) == 4);
    CHECK(threads_per_local_rank(6, 48, 8) == 6);
    // Not bound: every rank sees the whole node, so the node is split.
    CHECK(threads_per_local_rank(48, 48, 8) == 6);
    CHECK(threads_per_local_rank(48, 48, 1) == 48);
    // Never 0, also with more ranks than cores or no rank count.
    CHECK(threads_per_local_rank(4, 4, 8) == 1);
    CHECK(threads_per_local_rank(4, 4, 0) == 4);
}

#if defined(__linux__)
TEST_CASE("the default worker count follows the CPU affinity") {
    // The override wins over the affinity, and the Valgrind runner sets it.
    constexpr const char* OVERRIDE = "DFTRACER_UTILS_HW_CONCURRENCY";
    dstd::Optional<dstd::String> kept;
    if (const auto v = dftracer::utils::common::Env::get(OVERRIDE))
        kept = dstd::String(*v);
    ::unsetenv(OVERRIDE);

    cpu_set_t saved;
    CPU_ZERO(&saved);
    REQUIRE(sched_getaffinity(0, sizeof(saved), &saved) == 0);
    int first = -1;
    for (int c = 0; c < CPU_SETSIZE; ++c) {
        if (CPU_ISSET(c, &saved)) {
            first = c;
            break;
        }
    }
    REQUIRE(first >= 0);

    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(first, &one);
    REQUIRE(sched_setaffinity(0, sizeof(one), &one) == 0);
    const auto narrowed = dftracer::utils::common::hardware_concurrency();
    REQUIRE(sched_setaffinity(0, sizeof(saved), &saved) == 0);

    CHECK(narrowed == 1);
    CHECK(dftracer::utils::common::hardware_concurrency() ==
          static_cast<std::size_t>(CPU_COUNT(&saved)));
    if (kept) ::setenv(OVERRIDE, kept->c_str(), 1);
}
#endif
