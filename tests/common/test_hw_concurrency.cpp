#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/platform_compat.h>
#include <doctest/doctest.h>

#if defined(__linux__)
#include <sched.h>
#endif

TEST_CASE("the default worker count is never 0") {
    CHECK(dftracer::utils::hardware_concurrency() >= 1);
}

#if defined(__linux__)
TEST_CASE("the default worker count follows the CPU affinity") {
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
    const auto narrowed = dftracer::utils::hardware_concurrency();
    REQUIRE(sched_setaffinity(0, sizeof(saved), &saved) == 0);

    CHECK(narrowed == 1);
    CHECK(dftracer::utils::hardware_concurrency() ==
          static_cast<std::size_t>(CPU_COUNT(&saved)));
}
#endif
