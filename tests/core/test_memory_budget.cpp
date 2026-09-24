#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/memory_budget.h>
#include <doctest/doctest.h>

using dftracer::utils::format_memory_budget_warning;
using dftracer::utils::memory_budget_advice;
using dftracer::utils::PEAK_MEMORY_FACTOR;

TEST_SUITE("memory_budget_advice") {
    TEST_CASE("fits when peak is within available memory") {
        // 1 GB aggregated, 8 GB available: peak = 3 GB <= 8 GB.
        auto a = memory_budget_advice(1ULL << 30, 8ULL << 30);
        CHECK(a.fits);
        CHECK(a.peak_bytes == (3ULL << 30));
        CHECK(a.suggested_nodes == 1);
        CHECK(format_memory_budget_warning(a).empty());
    }

    TEST_CASE("does not fit when peak exceeds available, and advises nodes") {
        // 4 GB aggregated, 6 GB available: peak = 12 GB > 6 GB.
        auto a = memory_budget_advice(4ULL << 30, 6ULL << 30);
        CHECK_FALSE(a.fits);
        CHECK(a.peak_bytes == (12ULL << 30));
        // ceil(12 / 6) = 2 nodes.
        CHECK(a.suggested_nodes == 2);
        const std::string msg = format_memory_budget_warning(a);
        CHECK(msg.find("12.0 GB") != std::string::npos);
        CHECK(msg.find(">=2 nodes") != std::string::npos);
    }

    TEST_CASE("suggested_nodes rounds up") {
        // peak = 3 * 5 = 15 GB, available 4 GB: ceil(15/4) = 4.
        auto a = memory_budget_advice(5ULL << 30, 4ULL << 30);
        CHECK(a.suggested_nodes == 4);
    }

    TEST_CASE("peak is exactly PEAK_MEMORY_FACTOR times required") {
        auto a = memory_budget_advice(100, 1ULL << 30);
        CHECK(a.peak_bytes == 100 * PEAK_MEMORY_FACTOR);
    }
}

TEST_SUITE("concurrent spill budget") {
    using dftracer::utils::concurrent_spill_ways;
    using dftracer::utils::MIN_MEMORY_BUDGET_BYTES;
    using dftracer::utils::NO_SPILL_BUDGET;
    using dftracer::utils::share_spill_budget;

    TEST_CASE("shares of concurrent runs stay within the budget") {
        const std::uint64_t gib = 1ULL << 30;
        CHECK(share_spill_budget(gib, 4) == gib / 4);
        CHECK(share_spill_budget(gib, 3) * 3 <= gib);
        // Auto resolves against free memory, which moves between calls, so
        // only check that a split auto budget becomes a real share.
        CHECK(share_spill_budget(0, 2) > 0);
    }

    TEST_CASE("one run, never-spill and tiny budgets keep their meaning") {
        CHECK(share_spill_budget(0, 1) == 0);
        CHECK(share_spill_budget(1234, 1) == 1234);
        CHECK(share_spill_budget(NO_SPILL_BUDGET, 8) == NO_SPILL_BUDGET);
        CHECK(share_spill_budget(1, 4) == 1);
    }

    TEST_CASE("concurrency is capped so each share is at least the minimum") {
        CHECK(concurrent_spill_ways(4 * MIN_MEMORY_BUDGET_BYTES, 10) == 4);
        CHECK(concurrent_spill_ways(4 * MIN_MEMORY_BUDGET_BYTES, 2) == 2);
        CHECK(concurrent_spill_ways(1, 10) == 1);
        CHECK(concurrent_spill_ways(NO_SPILL_BUDGET, 10) == 10);
        CHECK(concurrent_spill_ways(4 * MIN_MEMORY_BUDGET_BYTES, 0) == 1);
    }
}
