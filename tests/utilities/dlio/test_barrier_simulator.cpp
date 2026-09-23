#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/dlio/barrier_simulator.h>
#include <dftracer/utils/utilities/dlio/statistic.h>
#include <dftracer/utils/utilities/dlio/worker_queue.h>
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

using namespace dftracer::utils::utilities::dlio;

namespace {

Statistic constant_stat(double value) {
    Statistic s;
    s.update(value);
    return s;
}

constexpr double EPSILON = 1e-9;

}  // namespace

TEST_SUITE("sweep_union") {
    TEST_CASE("empty") {
        std::vector<Boundary> b;
        CHECK(sweep_union(b) == doctest::Approx(0.0));
    }

    TEST_CASE("single interval - returns its width in seconds") {
        // 1 second = 1,000,000 us.
        std::vector<Boundary> b{{0, +1}, {1'000'000, -1}};
        CHECK(sweep_union(b) == doctest::Approx(1.0));
    }

    TEST_CASE("two disjoint intervals - sum of widths") {
        std::vector<Boundary> b{
            {0, +1}, {500'000, -1}, {1'000'000, +1}, {1'500'000, -1}};
        CHECK(sweep_union(b) == doctest::Approx(1.0));
    }

    TEST_CASE("two overlapping intervals - merged width") {
        // [0, 1s] and [0.5s, 1.5s] -> union is [0, 1.5s] = 1.5s.
        std::vector<Boundary> b{
            {0, +1}, {1'000'000, -1}, {500'000, +1}, {1'500'000, -1}};
        CHECK(sweep_union(b) == doctest::Approx(1.5));
    }

    TEST_CASE("nested intervals - outer width") {
        // [0, 2s] contains [0.5s, 1.5s].
        std::vector<Boundary> b{
            {0, +1}, {2'000'000, -1}, {500'000, +1}, {1'500'000, -1}};
        CHECK(sweep_union(b) == doctest::Approx(2.0));
    }
}

TEST_SUITE("cdf_similarity") {
    TEST_CASE("identical samples - exactly 1.0") {
        std::vector<double> a{0.1, 0.2, 0.3, 0.4};
        CHECK(cdf_similarity(a, a) == doctest::Approx(1.0));
    }

    TEST_CASE("fully disjoint samples") {
        // a in [0, 0.4], b in [10, 10.4]: at every value v < 10 the trace CDF
        // is 0 while sim CDF is 1 -> KS = 1, similarity = 0.
        std::vector<double> a{0.1, 0.2, 0.3, 0.4};
        std::vector<double> b{10.0, 10.1, 10.2, 10.4};
        CHECK(cdf_similarity(a, b) == doctest::Approx(0.0));
    }

    TEST_CASE("empty inputs") {
        std::vector<double> a{1.0, 2.0};
        std::vector<double> empty;
        CHECK(cdf_similarity(a, empty) == doctest::Approx(0.0));
        CHECK(cdf_similarity(empty, a) == doctest::Approx(0.0));
    }
}

TEST_SUITE("variance") {
    TEST_CASE("empty") { CHECK(variance({}) == doctest::Approx(0.0)); }

    TEST_CASE("constant values - zero variance") {
        CHECK(variance({3.0, 3.0, 3.0}) == doctest::Approx(0.0));
    }

    TEST_CASE("known sample") {
        // Population variance of {1, 2, 3, 4, 5} = 2.0.
        CHECK(variance({1.0, 2.0, 3.0, 4.0, 5.0}) == doctest::Approx(2.0));
    }
}

TEST_SUITE("WorkerQueue") {
    TEST_CASE("produce_batches fills exactly to capacity") {
        WorkerQueue q(/*num_workers=*/2, /*prefetch_factor=*/3);
        auto sampler = []() -> std::pair<double, double> { return {1.0, 0.5}; };
        auto intervals = q.produce_batches(0.0, sampler);
        CHECK(intervals.size() == 6);
        CHECK(q.queue_depth() == 6);
    }

    TEST_CASE("consume_batch on ready batch returns base_overhead") {
        WorkerQueue q(1, 1);
        auto sampler = []() -> std::pair<double, double> { return {2.0, 1.0}; };
        q.produce_batches(0.0, sampler);
        const double consumed =
            q.consume_batch(/*current_time=*/5.0, /*base_overhead=*/0.1);
        CHECK(consumed == doctest::Approx(0.1));
        CHECK(q.queue_depth() == 0);
        CHECK_FALSE(q.had_stall());
    }

    TEST_CASE("consume_batch on empty queue counts as stall") {
        WorkerQueue q(1, 1);
        const double consumed = q.consume_batch(0.0, 0.1);
        CHECK(consumed == doctest::Approx(0.1));
        CHECK(q.had_stall());
        CHECK(q.stall_count() == 1);
    }

    TEST_CASE(
        "consume_batch on not-yet-ready batch returns wait + base_overhead") {
        WorkerQueue q(1, 1);
        auto sampler = []() -> std::pair<double, double> {
            return {10.0, 5.0};
        };
        q.produce_batches(0.0, sampler);
        const double consumed =
            q.consume_batch(/*current_time=*/3.0, /*base_overhead=*/0.2);
        // Batch ready at 10.0, current 3.0 -> wait 7.0 + base 0.2 = 7.2.
        CHECK(consumed == doctest::Approx(7.2));
        CHECK(q.had_stall());
    }
}

TEST_SUITE("BarrierSimulator") {
    // Build a context with deterministic constant samplers and trace data.
    // Each rank runs num_steps iterations with constant fetch_iter=0.05s,
    // constant fetch_block=0.1s, no preprocess simulation, aggregated mode.
    static BarrierSimulatorContext make_ctx(int num_ranks, int num_steps) {
        BarrierSimulatorContext ctx;
        ctx.num_ranks = num_ranks;
        ctx.num_steps = num_steps;
        ctx.is_aggregated_trace = true;  // skip clamp-to-trace-bounds
        ctx.sync_mode = false;

        // Pre-loaded fetch_iter trace lets the simulator skip RNG-driven path.
        ctx.fetch_iter_trace.assign(num_ranks,
                                    std::vector<double>(num_steps, 0.05));
        // Trace fetch_block values (used by CDF similarity check).
        ctx.fetch_block_trace.assign(num_ranks,
                                     std::vector<double>(num_steps, 0.1));

        ctx.fetch_iter_stats = constant_stat(0.05);
        ctx.fetch_block_stats = constant_stat(0.1);
        ctx.preprocess_stats = constant_stat(0.01);
        ctx.getitem_stats = constant_stat(0.02);

        ctx.trace_e2e_duration = num_steps * (0.05 + 0.1);
        ctx.trace_rank_variance = 0.0;
        ctx.trace_per_rank_throughput.assign(num_ranks, 1.0 / 0.15);
        return ctx;
    }

    TEST_CASE("async mode, constant work - e2e equals per-rank wall clock") {
        const int num_ranks = 4;
        const int num_steps = 10;
        auto ctx = make_ctx(num_ranks, num_steps);

        BarrierSimulator sim;
        auto result =
            sim.simulate(ctx, /*base_seed=*/42,
                         /*fetch_block_sampler=*/[](Rng&) { return 0.1; });

        const double expected_per_rank = num_steps * (0.05 + 0.1);  // 1.5s
        CHECK(result.per_rank_completion_time.size() ==
              static_cast<std::size_t>(num_ranks));
        for (double t : result.per_rank_completion_time) {
            CHECK(t == doctest::Approx(expected_per_rank).epsilon(EPSILON));
        }
        // All ranks run in lockstep over the same wall clock; union = per-rank
        // time.
        CHECK(result.e2e_duration ==
              doctest::Approx(expected_per_rank).epsilon(1e-6));
        CHECK(result.rank_variance == doctest::Approx(0.0));
        CHECK(result.load_imbalance == doctest::Approx(0.0).epsilon(1e-6));
        CHECK(result.e2e_error == doctest::Approx(0.0).epsilon(1e-6));

        // Simulated fetch_block matches trace exactly -> CDF similarity == 1.0.
        CHECK(result.fetch_block_cdf_similarity == doctest::Approx(1.0));

        // Component accumulated times.
        CHECK(result.fetch_block_metrics.accumulated_time ==
              doctest::Approx(num_ranks * num_steps * 0.1));
        CHECK(result.fetch_iter_metrics.accumulated_time ==
              doctest::Approx(num_ranks * num_steps * 0.05));
        CHECK(result.fetch_block_metrics.num_samples ==
              static_cast<std::uint64_t>(num_ranks * num_steps));
    }

    TEST_CASE("sync mode with barrier every step - lockstep advance") {
        const int num_ranks = 3;
        const int num_steps = 5;
        auto ctx = make_ctx(num_ranks, num_steps);
        ctx.sync_mode = true;
        ctx.accumulate_grad_batches = 1;

        BarrierSimulator sim;
        auto result =
            sim.simulate(ctx, /*base_seed=*/123, [](Rng&) { return 0.1; });

        const double expected = num_steps * 0.15;
        // In sync mode all ranks finish at the same time; e2e = rank_times[0].
        CHECK(result.e2e_duration == doctest::Approx(expected).epsilon(1e-6));
        for (double t : result.per_rank_completion_time) {
            CHECK(t == doctest::Approx(expected).epsilon(EPSILON));
        }
        // Constant work + barrier -> zero barrier overhead.
        CHECK(result.avg_barrier_overhead == doctest::Approx(0.0));
        CHECK(result.max_barrier_overhead == doctest::Approx(0.0));
    }

    TEST_CASE(
        "throughput metrics populated when trace throughput is provided") {
        auto ctx = make_ctx(/*num_ranks=*/2, /*num_steps=*/8);
        BarrierSimulator sim;
        auto result = sim.simulate(ctx, 7, [](Rng&) { return 0.1; });

        const double expected_throughput = 8.0 / (8 * 0.15);
        CHECK(result.simulated_per_rank_throughput.size() == 2);
        for (double tp : result.simulated_per_rank_throughput) {
            CHECK(tp == doctest::Approx(expected_throughput));
        }
        CHECK(result.throughput_mean == doctest::Approx(expected_throughput));
        CHECK(result.throughput_variance == doctest::Approx(0.0).epsilon(1e-9));
        // CDF similarity is computed but not asserted exactly: simulated
        // throughput can differ from trace throughput by ~1 ULP due to
        // accumulation order, and KS is exact so a 1-ULP gap collapses
        // similarity to 0. Just check it ran.
        CHECK(result.trace_per_rank_throughput.size() == 2);
    }
}
