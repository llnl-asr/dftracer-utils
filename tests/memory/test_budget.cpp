#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/budget.h>
#include <doctest/doctest.h>

#include <cstdint>

using dftracer::utils::memory::detect_available_memory;
using dftracer::utils::memory::MIN_MEMORY_BUDGET_BYTES;
using dftracer::utils::memory::NO_SPILL_BUDGET;
using dftracer::utils::memory::resolve_spill_budget;

TEST_CASE("detection returns a nonzero value") {
    CHECK(detect_available_memory() > 0);
}

TEST_CASE("auto resolves to a third of available memory with a floor") {
    // Free memory moves between calls, so bound the result by two readings.
    const std::uint64_t before = detect_available_memory() / 3;
    const std::uint64_t got = resolve_spill_budget(0);
    const std::uint64_t after = detect_available_memory() / 3;
    CHECK(got >= MIN_MEMORY_BUDGET_BYTES);
    const std::uint64_t lo = before < after ? before : after;
    const std::uint64_t hi = before < after ? after : before;
    if (hi >= MIN_MEMORY_BUDGET_BYTES) {
        CHECK(got >= lo / 2);
        CHECK(got <= hi * 2);
    } else {
        CHECK(got == MIN_MEMORY_BUDGET_BYTES);
    }
}

TEST_CASE("never-spill passes through") {
    CHECK(resolve_spill_budget(NO_SPILL_BUDGET) == NO_SPILL_BUDGET);
}

TEST_CASE("explicit values stay") {
    CHECK(resolve_spill_budget(1) == 1);
    CHECK(resolve_spill_budget(12345) == 12345);
    CHECK(resolve_spill_budget(MIN_MEMORY_BUDGET_BYTES / 2) ==
          MIN_MEMORY_BUDGET_BYTES / 2);
}
