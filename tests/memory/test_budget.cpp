// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/memory/budget.h>
#include <doctest/doctest.h>

#include <cstdint>

using namespace dftracer::utils;

TEST_CASE("detection returns a nonzero value") {
    CHECK(memory::detect_available_memory() > 0);
}

TEST_CASE("auto resolves to a third of available memory with a floor") {
    // Free memory moves between calls, so bound the result by two readings.
    const std::uint64_t before = memory::detect_available_memory() / 3;
    const std::uint64_t got = memory::resolve_spill_budget(0);
    const std::uint64_t after = memory::detect_available_memory() / 3;
    CHECK(got >= memory::MIN_MEMORY_BUDGET_BYTES);
    const std::uint64_t lo = before < after ? before : after;
    const std::uint64_t hi = before < after ? after : before;
    if (hi >= memory::MIN_MEMORY_BUDGET_BYTES) {
        CHECK(got >= lo / 2);
        CHECK(got <= hi * 2);
    } else {
        CHECK(got == memory::MIN_MEMORY_BUDGET_BYTES);
    }
}

TEST_CASE("never-spill passes through") {
    CHECK(memory::resolve_spill_budget(memory::NO_SPILL_BUDGET) ==
          memory::NO_SPILL_BUDGET);
}

TEST_CASE("explicit values stay") {
    CHECK(memory::resolve_spill_budget(1) == 1);
    CHECK(memory::resolve_spill_budget(12345) == 12345);
    CHECK(memory::resolve_spill_budget(memory::MIN_MEMORY_BUDGET_BYTES / 2) ==
          memory::MIN_MEMORY_BUDGET_BYTES / 2);
}
