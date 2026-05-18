#include <dftracer/utils/utilities/dlio/barrier_simulator.h>
#include <dftracer/utils/utilities/dlio/statistic.h>
#include <dftracer/utils/utilities/dlio/worker_queue.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace dftracer::utils::utilities::dlio {

namespace {

constexpr std::uint64_t PREPROCESS_RNG_OFFSET = 888888;
constexpr std::uint64_t STEP_RNG_OFFSET = 999999;

inline double uniform01(Rng& rng) {
    return std::uniform_real_distribution<double>(0.0, 1.0)(rng);
}

}  // namespace

double sweep_union(std::vector<Boundary>& boundaries) {
    if (boundaries.empty()) return 0.0;
    std::sort(
        boundaries.begin(), boundaries.end(),
        [](const Boundary& a, const Boundary& b) { return a.time < b.time; });

    double union_time_us = 0.0;
    int active = 0;
    std::int64_t last_time = 0;
    for (const auto& b : boundaries) {
        if (active > 0)
            union_time_us += static_cast<double>(b.time - last_time);
        active += b.delta;
        last_time = b.time;
    }
    return union_time_us / 1e6;
}

double variance(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    double mean = 0.0;
    for (double v : values) mean += v;
    mean /= static_cast<double>(values.size());

    double sq = 0.0;
    for (double v : values) {
        double d = v - mean;
        sq += d * d;
    }
    return sq / static_cast<double>(values.size());
}

double cdf_similarity(const std::vector<double>& a,
                      const std::vector<double>& b) {
    if (a.empty() || b.empty()) return 0.0;

    std::vector<double> as(a), bs(b);
    std::sort(as.begin(), as.end());
    std::sort(bs.begin(), bs.end());

    const auto na = static_cast<double>(as.size());
    const auto nb = static_cast<double>(bs.size());

    std::vector<double> all;
    all.reserve(as.size() + bs.size());
    all.insert(all.end(), as.begin(), as.end());
    all.insert(all.end(), bs.begin(), bs.end());
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());

    double max_diff = 0.0;
    for (double v : all) {
        const auto ca = static_cast<double>(
            std::upper_bound(as.begin(), as.end(), v) - as.begin());
        const auto cb = static_cast<double>(
            std::upper_bound(bs.begin(), bs.end(), v) - bs.begin());
        double diff = std::abs(ca / na - cb / nb);
        if (diff > max_diff) max_diff = diff;
    }
    return 1.0 - max_diff;
}

std::vector<WorkInterval> WorkerQueue::produce_batches(
    double current_time, const BatchTimeSampler& sampler) {
    if (worker_free_times_.empty()) {
        worker_free_times_.assign(static_cast<std::size_t>(num_workers_),
                                  current_time);
    }

    std::vector<WorkInterval> intervals;
    while (ready_batches_.size() < queue_capacity_) {
        const auto earliest_it = std::min_element(worker_free_times_.begin(),
                                                  worker_free_times_.end());
        const double worker_available = *earliest_it;

        auto [batch_time, preprocess_time] = sampler();
        const double batch_ready = worker_available + batch_time;
        intervals.push_back({worker_available, batch_ready, preprocess_time});

        *earliest_it = batch_ready;
        ready_batches_.push_back(batch_ready);
    }
    std::sort(ready_batches_.begin(), ready_batches_.end());
    return intervals;
}

double WorkerQueue::consume_batch(double current_time, double base_overhead) {
    if (ready_batches_.empty()) {
        ++stall_count_;
        return base_overhead;
    }
    const double batch_ready = ready_batches_.front();
    ready_batches_.erase(ready_batches_.begin());

    if (batch_ready <= current_time) return base_overhead;

    ++stall_count_;
    return (batch_ready - current_time) + base_overhead;
}

BarrierSimulationResult BarrierSimulator::simulate(
    const BarrierSimulatorContext& ctx, std::uint64_t base_seed,
    const Sampler& fetch_block_sampler,
    const Sampler& preprocess_sampler) const {
    BarrierSimulationResult result;

    std::vector<Rng> rank_rngs;
    rank_rngs.reserve(static_cast<std::size_t>(ctx.num_ranks));
    for (int rank = 0; rank < ctx.num_ranks; ++rank) {
        rank_rngs.emplace_back(base_seed + static_cast<std::uint64_t>(rank));
    }

    std::vector<double> rank_times(static_cast<std::size_t>(ctx.num_ranks),
                                   0.0);
    std::vector<double> rank_work_times(static_cast<std::size_t>(ctx.num_ranks),
                                        0.0);
    std::vector<double> barrier_overheads;

    std::vector<Boundary> boundaries;
    std::vector<Boundary> preprocess_boundaries;
    std::vector<Boundary> fetch_iter_boundaries;
    std::vector<Boundary> fetch_block_boundaries;

    std::vector<WorkerQueue> rank_queues;
    std::vector<Rng> preprocess_rngs;
    preprocess_rngs.reserve(static_cast<std::size_t>(ctx.num_ranks));
    for (int rank = 0; rank < ctx.num_ranks; ++rank) {
        preprocess_rngs.emplace_back(base_seed + PREPROCESS_RNG_OFFSET +
                                     static_cast<std::uint64_t>(rank));
    }

    auto worker_batch_sampler_for = [&](int rank, bool record_simulated) {
        return [&, rank, record_simulated]() -> std::pair<double, double> {
            Rng& rrng = preprocess_rngs[static_cast<std::size_t>(rank)];
            double sampled_preprocess = 0.0;
            double sampled_getitem = 0.0;
            if (preprocess_sampler && ctx.io_stats) {
                sampled_preprocess = preprocess_sampler(rrng);
                const double sampled_io =
                    ctx.io_stats->quantile(uniform01(rrng));
                sampled_getitem = sampled_preprocess + sampled_io;
            } else {
                sampled_getitem = ctx.getitem_stats.quantile(uniform01(rrng));
                sampled_preprocess =
                    ctx.preprocess_stats.quantile(uniform01(rrng));
            }

            const double io_time =
                std::max(0.0, sampled_getitem - sampled_preprocess);
            const double total_time = io_time + sampled_preprocess;
            const double adjusted_time =
                total_time * ctx.preprocess_slowdown_factor;

            if (record_simulated) {
                result.simulated_preprocess.push_back(sampled_preprocess);
                result.simulated_getitem.push_back(sampled_getitem);
            }

            result.preprocess_metrics.accumulated_time += sampled_preprocess;
            ++result.preprocess_metrics.num_samples;
            result.preprocess_metrics.stats.update(sampled_preprocess);

            return {adjusted_time, sampled_preprocess};
        };
    };

    if (ctx.enable_preprocess_simulation) {
        for (int rank = 0; rank < ctx.num_ranks; ++rank) {
            rank_queues.emplace_back(ctx.num_workers, ctx.prefetch_factor);
        }
        for (int rank = 0; rank < ctx.num_ranks; ++rank) {
            auto sampler =
                worker_batch_sampler_for(rank, /*record_simulated=*/false);
            auto intervals =
                rank_queues[static_cast<std::size_t>(rank)].produce_batches(
                    0.0, sampler);
            for (const auto& interval : intervals) {
                preprocess_boundaries.push_back(
                    {static_cast<std::int64_t>(interval.start_time), +1});
                preprocess_boundaries.push_back(
                    {static_cast<std::int64_t>(interval.end_time), -1});
            }
        }
    }

    Rng step_rng(base_seed + STEP_RNG_OFFSET);

    std::uint64_t total_queue_stalls = 0;
    std::uint64_t total_queue_depth_samples = 0;
    double sum_queue_depth = 0.0;

    for (int step = 0; step < ctx.num_steps; ++step) {
        for (int rank = 0; rank < ctx.num_ranks; ++rank) {
            const auto r = static_cast<std::size_t>(rank);
            double fetch_iter = 0.0;
            double fetch_block = 0.0;

            const bool have_aggregated_fetch_iter =
                ctx.is_aggregated_trace && !ctx.enable_preprocess_simulation &&
                static_cast<int>(ctx.fetch_iter_trace.size()) > rank &&
                static_cast<int>(ctx.fetch_iter_trace[r].size()) > step;

            if (have_aggregated_fetch_iter) {
                fetch_iter =
                    ctx.fetch_iter_trace[r][static_cast<std::size_t>(step)];
            } else if (ctx.enable_preprocess_simulation) {
                auto& queue = rank_queues[r];

                fetch_iter = queue.consume_batch(rank_times[r],
                                                 ctx.base_fetch_iter_overhead);
                result.simulated_fetch_iter.push_back(fetch_iter);
                if (queue.had_stall()) ++total_queue_stalls;
                sum_queue_depth += static_cast<double>(queue.queue_depth());
                ++total_queue_depth_samples;

                auto sampler =
                    worker_batch_sampler_for(rank, /*record_simulated=*/true);
                auto intervals =
                    queue.produce_batches(rank_times[r] + fetch_iter, sampler);
                for (const auto& interval : intervals) {
                    preprocess_boundaries.push_back(
                        {static_cast<std::int64_t>(interval.start_time), +1});
                    preprocess_boundaries.push_back(
                        {static_cast<std::int64_t>(interval.end_time), -1});
                }
            } else {
                fetch_iter = ctx.fetch_iter_stats.quantile(uniform01(step_rng));
                if (!ctx.is_aggregated_trace) {
                    fetch_iter =
                        std::clamp(fetch_iter, ctx.fetch_iter_stats.min(),
                                   ctx.fetch_iter_stats.max());
                }
            }

            fetch_block = fetch_block_sampler(rank_rngs[r]);
            if (!ctx.is_aggregated_trace) {
                fetch_block =
                    std::clamp(fetch_block, ctx.fetch_block_stats.min(),
                               ctx.fetch_block_stats.max());
            }

            result.simulated_fetch_block.push_back(fetch_block);
            rank_work_times[r] += fetch_block + fetch_iter;

            const double start_time = rank_times[r];
            const double fetch_iter_end_time = start_time + fetch_iter;
            const double end_time = fetch_iter_end_time + fetch_block;

            result.fetch_iter_metrics.accumulated_time += fetch_iter;
            ++result.fetch_iter_metrics.num_samples;
            result.fetch_iter_metrics.stats.update(fetch_iter);
            fetch_iter_boundaries.push_back(
                {static_cast<std::int64_t>(start_time * 1e6), +1});
            fetch_iter_boundaries.push_back(
                {static_cast<std::int64_t>(fetch_iter_end_time * 1e6), -1});

            result.fetch_block_metrics.accumulated_time += fetch_block;
            ++result.fetch_block_metrics.num_samples;
            result.fetch_block_metrics.stats.update(fetch_block);
            fetch_block_boundaries.push_back(
                {static_cast<std::int64_t>(fetch_iter_end_time * 1e6), +1});
            fetch_block_boundaries.push_back(
                {static_cast<std::int64_t>(end_time * 1e6), -1});

            if (!ctx.sync_mode) {
                boundaries.push_back(
                    {static_cast<std::int64_t>(start_time * 1e6), +1});
                boundaries.push_back(
                    {static_cast<std::int64_t>(end_time * 1e6), -1});
            }

            rank_times[r] = end_time;
        }

        const bool is_barrier_step =
            ctx.accumulate_grad_batches > 0 &&
            ((step + 1) % ctx.accumulate_grad_batches == 0);
        if (ctx.sync_mode && is_barrier_step) {
            const double max_time =
                *std::max_element(rank_times.begin(), rank_times.end());
            for (int rank = 0; rank < ctx.num_ranks; ++rank) {
                const auto r = static_cast<std::size_t>(rank);
                barrier_overheads.push_back(max_time - rank_times[r]);
                rank_times[r] = max_time;
            }
        }
    }

    if (ctx.sync_mode) {
        result.e2e_duration = rank_times.empty() ? 0.0 : rank_times.front();
    } else if (!boundaries.empty()) {
        result.e2e_duration = sweep_union(boundaries);
    } else if (!rank_times.empty()) {
        result.e2e_duration =
            *std::max_element(rank_times.begin(), rank_times.end());
    }

    if (!preprocess_boundaries.empty())
        result.preprocess_metrics.union_time =
            sweep_union(preprocess_boundaries);
    if (!fetch_iter_boundaries.empty())
        result.fetch_iter_metrics.union_time =
            sweep_union(fetch_iter_boundaries);
    if (!fetch_block_boundaries.empty())
        result.fetch_block_metrics.union_time =
            sweep_union(fetch_block_boundaries);

    result.trace_preprocess_metrics = ctx.trace_preprocess_metrics;
    result.trace_fetch_iter_metrics = ctx.trace_fetch_iter_metrics;
    result.trace_fetch_block_metrics = ctx.trace_fetch_block_metrics;

    if (!barrier_overheads.empty()) {
        double sum = 0.0;
        double max_ov = -std::numeric_limits<double>::infinity();
        for (double v : barrier_overheads) {
            sum += v;
            if (v > max_ov) max_ov = v;
        }
        result.avg_barrier_overhead =
            sum / static_cast<double>(barrier_overheads.size());
        result.max_barrier_overhead = max_ov;
    }

    {
        std::vector<double> trace_flat;
        for (const auto& rank_data : ctx.fetch_block_trace) {
            trace_flat.insert(trace_flat.end(), rank_data.begin(),
                              rank_data.end());
        }
        result.fetch_block_cdf_similarity =
            cdf_similarity(result.simulated_fetch_block, trace_flat);
    }

    if (!result.simulated_fetch_iter.empty() && !ctx.fetch_iter_trace.empty()) {
        std::vector<double> trace_flat;
        for (const auto& rank_data : ctx.fetch_iter_trace) {
            trace_flat.insert(trace_flat.end(), rank_data.begin(),
                              rank_data.end());
        }
        result.fetch_iter_cdf_similarity =
            cdf_similarity(result.simulated_fetch_iter, trace_flat);
    }

    if (!result.simulated_getitem.empty() && ctx.getitem_trace) {
        std::vector<double> trace_flat;
        for (const auto& rank_data : *ctx.getitem_trace) {
            trace_flat.insert(trace_flat.end(), rank_data.begin(),
                              rank_data.end());
        }
        result.getitem_cdf_similarity =
            cdf_similarity(result.simulated_getitem, trace_flat);
    }

    if (ctx.trace_e2e_duration > 0.0) {
        result.e2e_error =
            std::abs(result.e2e_duration - ctx.trace_e2e_duration) /
            ctx.trace_e2e_duration;
    }

    result.per_rank_completion_time = rank_times;
    result.rank_variance = variance(rank_work_times);
    result.trace_rank_variance = ctx.trace_rank_variance;
    if (ctx.trace_rank_variance > 0.0) {
        result.rank_variance_error =
            std::abs(result.rank_variance - ctx.trace_rank_variance) /
            ctx.trace_rank_variance;
    }

    if (!rank_times.empty()) {
        const double min_t =
            *std::min_element(rank_times.begin(), rank_times.end());
        const double max_t =
            *std::max_element(rank_times.begin(), rank_times.end());
        result.load_imbalance = (max_t - min_t) / (min_t + 1e-9);
    }

    if (ctx.enable_preprocess_simulation && total_queue_depth_samples > 0) {
        result.avg_queue_depth =
            sum_queue_depth / static_cast<double>(total_queue_depth_samples);
        result.avg_queue_stalls =
            static_cast<double>(total_queue_stalls) /
            static_cast<double>(ctx.num_ranks * ctx.num_steps);
    }

    result.simulated_per_rank_throughput.reserve(
        static_cast<std::size_t>(ctx.num_ranks));
    for (int rank = 0; rank < ctx.num_ranks; ++rank) {
        const auto r = static_cast<std::size_t>(rank);
        result.simulated_per_rank_throughput.push_back(
            rank_times[r] > 0.0
                ? static_cast<double>(ctx.num_steps) / rank_times[r]
                : 0.0);
    }
    result.trace_per_rank_throughput = ctx.trace_per_rank_throughput;

    if (!result.simulated_per_rank_throughput.empty() &&
        !result.trace_per_rank_throughput.empty()) {
        double sim_sum = 0.0;
        for (double v : result.simulated_per_rank_throughput) sim_sum += v;
        result.throughput_mean =
            sim_sum /
            static_cast<double>(result.simulated_per_rank_throughput.size());

        double tr_sum = 0.0;
        for (double v : result.trace_per_rank_throughput) tr_sum += v;
        result.trace_throughput_mean =
            tr_sum /
            static_cast<double>(result.trace_per_rank_throughput.size());

        if (result.trace_throughput_mean > 0.0) {
            result.throughput_mean_error =
                std::abs(result.throughput_mean -
                         result.trace_throughput_mean) /
                result.trace_throughput_mean;
        }
        result.throughput_variance =
            variance(result.simulated_per_rank_throughput);
        result.trace_throughput_variance =
            variance(result.trace_per_rank_throughput);
        result.throughput_cdf_similarity =
            cdf_similarity(result.simulated_per_rank_throughput,
                           result.trace_per_rank_throughput);
    }

    return result;
}

}  // namespace dftracer::utils::utilities::dlio
