#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/runtime/coro/async_semaphore.h>
#include <doctest/doctest.h>

using dftracer::utils::coro::CoroSemaphore;

TEST_CASE("try_acquire takes free permits and never waits") {
    CoroSemaphore sem(10);
    CHECK(sem.try_acquire(6));
    CHECK_FALSE(sem.try_acquire(5));
    CHECK(sem.try_acquire(4));
    CHECK_FALSE(sem.try_acquire(1));
    sem.release(10);
    CHECK(sem.try_acquire(10));
}

TEST_CASE("a release above the capacity returns the capacity") {
    // An acquire of more than the capacity takes the capacity, so the release
    // of the same request must not add permits past it.
    CoroSemaphore sem(10);
    CHECK(sem.try_acquire(10));
    sem.release(25);
    CHECK(sem.try_acquire(10));
    CHECK_FALSE(sem.try_acquire(1));
}
