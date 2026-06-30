#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/rocksdb/column_families.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_metrics.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/event_aggregator.h>
#include <dftracer/utils/utilities/dlio/trace_loader.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace dftracer::utils::utilities::dlio {

namespace {

namespace agg = ::dftracer::utils::utilities::composites::dft::aggregators;
namespace rdb = ::dftracer::utils::rocksdb;

constexpr double US_TO_S = 1e-6;

// AGGREGATION CF contains data keys (varint-encoded) and reserved system keys
// prefixed with 0xFF{FD,FE,FF}. Filter those out.
inline bool is_system_key(std::string_view key) {
    return key.size() >= 2 && static_cast<std::uint8_t>(key[0]) == 0xFF;
}

inline bool matches(std::string_view a, std::string_view b) { return a == b; }

struct ComponentAccumulator {
    // Per-(pid, time_bucket) entries used to materialize per-rank sample seqs.
    // Outer map sorts by pid for deterministic rank assignment; inner sorts by
    // time_bucket so the resulting sample sequence is in trace order.
    std::map<std::uint64_t, std::map<std::uint64_t, std::vector<double>>>
        per_pid_bucket_samples;
    // Boundary list (in microseconds) for sweep_union of trace-side wall clock.
    std::vector<Boundary> boundaries;
    // Merged sketch across all entries; nullptr until first sketch is seen.
    std::shared_ptr<DDSketch> sketch;
    // Aggregate accumulators.
    double accumulated_time_s = 0.0;  // sum of count * mean (s)
    std::uint64_t total_count = 0;
    double min_us = 0.0;
    double max_us = 0.0;
    bool min_max_seen = false;
};

void apply_minmax(ComponentAccumulator& acc, double min_us, double max_us) {
    if (!acc.min_max_seen) {
        acc.min_us = min_us;
        acc.max_us = max_us;
        acc.min_max_seen = true;
        return;
    }
    if (min_us < acc.min_us) acc.min_us = min_us;
    if (max_us > acc.max_us) acc.max_us = max_us;
}

// Synthesize per-call durations (seconds) for a single (cat, name, pid, bucket)
// entry. When a sketch is available we draw `n` samples via inverse-CDF for
// within-bucket variance; otherwise we replicate the per-call mean. `n` is
// clamped to `max_samples` if non-zero.
void synthesize_samples(const agg::AggregationMetrics& metrics,
                        std::uint64_t max_samples, std::mt19937_64& rng,
                        std::vector<double>& out) {
    if (metrics.count == 0) return;
    const auto desired =
        max_samples == 0 ? metrics.count : std::min(metrics.count, max_samples);
    if (desired == 0) return;

    const double mean_s = metrics.duration.mean * US_TO_S;
    if (metrics.duration.sketch) {
        std::uniform_real_distribution<double> u01(0.0, 1.0);
        out.reserve(out.size() + desired);
        for (std::uint64_t i = 0; i < desired; ++i) {
            const double q = u01(rng);
            const double v_us = metrics.duration.sketch->quantile(q);
            out.push_back(v_us * US_TO_S);
        }
    } else {
        out.insert(out.end(), desired, mean_s);
    }
}

// Drains the per-(pid, bucket) sample buckets in `acc` into per-rank flat
// vectors, in pid-ascending and then bucket-ascending order.
std::vector<std::vector<double>> flatten_per_rank(
    const ComponentAccumulator& acc,
    const std::vector<std::uint64_t>& rank_pids) {
    std::vector<std::vector<double>> out;
    out.reserve(rank_pids.size());
    for (auto pid : rank_pids) {
        std::vector<double> rank_samples;
        auto it = acc.per_pid_bucket_samples.find(pid);
        if (it == acc.per_pid_bucket_samples.end()) {
            out.emplace_back();
            continue;
        }
        for (const auto& [bucket, samples] : it->second) {
            (void)bucket;
            rank_samples.insert(rank_samples.end(), samples.begin(),
                                samples.end());
        }
        out.push_back(std::move(rank_samples));
    }
    return out;
}

}  // namespace

AggregatedTraces load_aggregated_traces(const std::string& db_path,
                                        const TraceLoaderOptions& options) {
    // The AGGREGATION CF was created with a merge operator; we must re-attach
    // it (read-only) so RocksDB will let us iterate the CF. Without the merge
    // operator, NewIterator() returns "merge_operator_ must be set".
    auto db_handle =
        agg::EventAggregator::open_read_only_with_merge_operator(db_path);
    if (!db_handle) {
        throw DFTUtilsException(ErrorCode::IO,
                                "dlio: failed to open RocksDB at " + db_path);
    }
    auto& db = *db_handle;

    // The intern dictionary must be populated before any key parsing happens.
    agg::load_intern_dictionary(db);

    AggregatedTraces out;

    // Global config (time_interval_us) lives at key 0xFFFE in AGGREGATION CF.
    {
        std::string val;
        const auto st = db.get(std::string_view(agg::AGG_GLOBAL_CONFIG_KEY, 2),
                               &val, rdb::cf::AGGREGATION);
        if (st.ok() && !val.empty()) {
            const auto cfg = agg::deserialize_agg_global_config(val);
            out.time_interval_us = cfg.time_interval_us;
        }
    }

    ComponentAccumulator acc_fetch_block;
    ComponentAccumulator acc_fetch_iter;
    ComponentAccumulator acc_preprocess;
    ComponentAccumulator acc_getitem;

    std::unordered_set<std::uint64_t> pid_set;
    std::mt19937_64 rng(options.seed);

    auto it = db.new_iterator(rdb::cf::AGGREGATION);
    if (!it) {
        throw DFTUtilsException(ErrorCode::IO,
                                "dlio: failed to obtain AGGREGATION iterator");
    }

    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        const auto key_slice = it->key();
        std::string_view key_sv(key_slice.data(), key_slice.size());
        if (is_system_key(key_sv)) continue;

        agg::AggKeyView kv;
        if (!agg::parse_agg_key_view(key_sv, kv)) continue;

        ComponentAccumulator* target = nullptr;
        if (matches(kv.cat, CATEGORY_DATALOADER) &&
            matches(kv.name, EVENT_FETCH_BLOCK)) {
            target = &acc_fetch_block;
        } else if (matches(kv.cat, CATEGORY_DATALOADER) &&
                   matches(kv.name, EVENT_FETCH_ITER)) {
            target = &acc_fetch_iter;
        } else if (matches(kv.cat, CATEGORY_DATA) &&
                   matches(kv.name, EVENT_PREPROCESS)) {
            target = &acc_preprocess;
        } else if (matches(kv.cat, CATEGORY_DATA) &&
                   matches(kv.name, EVENT_ITEM)) {
            target = &acc_getitem;
        } else {
            continue;
        }

        const auto val_slice = it->value();
        std::string_view val_sv(val_slice.data(), val_slice.size());
        auto metrics = agg::deserialize_agg_value(val_sv);
        if (metrics.count == 0) continue;

        out.any_data = true;
        pid_set.insert(kv.pid);

        // Synthesize per-call samples for this entry.
        auto& bucket_vec =
            target->per_pid_bucket_samples[kv.pid][kv.time_bucket];
        synthesize_samples(metrics, options.max_samples_per_entry, rng,
                           bucket_vec);

        // Accumulate component-level state.
        target->accumulated_time_s +=
            static_cast<double>(metrics.duration.total) * US_TO_S;
        target->total_count += metrics.count;
        apply_minmax(*target, static_cast<double>(metrics.duration.min),
                     static_cast<double>(metrics.duration.max));

        // Real per-entry (ts, te) interval for trace-side union time.
        if (metrics.te > metrics.ts) {
            target->boundaries.push_back(
                {static_cast<std::int64_t>(metrics.ts), +1});
            target->boundaries.push_back(
                {static_cast<std::int64_t>(metrics.te), -1});
        }

        // Merge sketch when present.
        if (metrics.duration.sketch) {
            out.sketches_available = true;
            if (!target->sketch) {
                target->sketch =
                    std::make_shared<DDSketch>(*metrics.duration.sketch);
            } else {
                target->sketch->merge(*metrics.duration.sketch);
            }
        }
    }

    if (!it->status().ok()) {
        throw DFTUtilsException(ErrorCode::IO,
                                "dlio: iteration over AGGREGATION CF failed: " +
                                    it->status().ToString());
    }

    if (!out.any_data) {
        return out;
    }

    // Rank PIDs are the pids that emitted at least one fetch.block event.
    // fetch_iter is intentionally not required since some traces omit it.
    std::vector<std::uint64_t> rank_pids;
    rank_pids.reserve(acc_fetch_block.per_pid_bucket_samples.size());
    for (const auto& [pid, _] : acc_fetch_block.per_pid_bucket_samples) {
        rank_pids.push_back(pid);
    }
    std::sort(rank_pids.begin(), rank_pids.end());
    out.rank_pids = rank_pids;
    out.num_ranks = static_cast<int>(rank_pids.size());

    // Per-rank traces.
    out.fetch_block_trace = flatten_per_rank(acc_fetch_block, rank_pids);
    out.fetch_iter_trace = flatten_per_rank(acc_fetch_iter, rank_pids);
    out.getitem_trace = flatten_per_rank(acc_getitem, rank_pids);

    // num_steps = min length across ranks (conservative — drop straggler
    // steps).
    out.num_steps = out.num_ranks > 0
                        ? static_cast<int>(out.fetch_block_trace.front().size())
                        : 0;
    for (const auto& r : out.fetch_block_trace) {
        out.num_steps = std::min(out.num_steps, static_cast<int>(r.size()));
    }

    // Flat fitting arrays.
    for (const auto& r : out.fetch_block_trace) {
        out.computation_times.insert(out.computation_times.end(), r.begin(),
                                     r.end());
    }
    for (const auto& [pid, buckets] : acc_preprocess.per_pid_bucket_samples) {
        (void)pid;
        for (const auto& [bucket, samples] : buckets) {
            (void)bucket;
            out.preprocess_times.insert(out.preprocess_times.end(),
                                        samples.begin(), samples.end());
        }
    }

    // Sketches + Statistic objects.
    auto attach_stat = [](Statistic& stat, const ComponentAccumulator& a) {
        if (!a.min_max_seen) return;
        // Seed Statistic with min and max (in seconds) so its fallback quantile
        // path has reasonable bounds even without a sketch.
        stat.update(a.min_us * US_TO_S);
        if (a.max_us != a.min_us) stat.update(a.max_us * US_TO_S);
        if (a.sketch) {
            stat.attach_sketch(a.sketch);
        }
    };
    attach_stat(out.fetch_block_stats, acc_fetch_block);
    attach_stat(out.fetch_iter_stats, acc_fetch_iter);
    attach_stat(out.preprocess_stats, acc_preprocess);
    attach_stat(out.getitem_stats, acc_getitem);
    out.fetch_block_sketch = acc_fetch_block.sketch;
    out.fetch_iter_sketch = acc_fetch_iter.sketch;
    out.preprocess_sketch = acc_preprocess.sketch;
    out.getitem_sketch = acc_getitem.sketch;

    // Trace-side ComponentTimeMetrics: accumulated + union (in seconds).
    auto fill_metrics = [](ComponentTimeMetrics& m, ComponentAccumulator& a) {
        m.accumulated_time = a.accumulated_time_s;
        m.num_samples = a.total_count;
        m.union_time = sweep_union(a.boundaries);  // returns seconds
    };
    fill_metrics(out.trace_fetch_block_metrics, acc_fetch_block);
    fill_metrics(out.trace_fetch_iter_metrics, acc_fetch_iter);
    fill_metrics(out.trace_preprocess_metrics, acc_preprocess);

    // Overall e2e: union of fetch_block intervals across all ranks gives the
    // tightest defensible estimate for "time spent in the data path".
    out.trace_e2e_duration = out.trace_fetch_block_metrics.union_time;

    // Per-rank throughput = events / rank-side fetch_block wall clock. We
    // approximate rank wall clock as sum(per-entry te-ts) for that rank's
    // fetch_block keys; matches the granularity available without raw events.
    out.trace_per_rank_throughput.reserve(rank_pids.size());
    std::vector<double> per_rank_wall;
    per_rank_wall.reserve(rank_pids.size());
    for (auto pid : rank_pids) {
        double wall_us = 0.0;
        std::uint64_t count = 0;
        auto pit = acc_fetch_block.per_pid_bucket_samples.find(pid);
        if (pit != acc_fetch_block.per_pid_bucket_samples.end()) {
            for (const auto& [bucket, samples] : pit->second) {
                (void)bucket;
                count += samples.size();
            }
            // Wall clock per rank = sum of sample durations as an upper bound.
            for (const auto& [bucket, samples] : pit->second) {
                (void)bucket;
                for (double s : samples) wall_us += s / US_TO_S;
            }
        }
        const double wall_s = wall_us * US_TO_S;
        per_rank_wall.push_back(wall_s);
        out.trace_per_rank_throughput.push_back(
            wall_s > 0.0 ? static_cast<double>(count) / wall_s : 0.0);
    }

    // Trace rank variance = variance of per-rank wall clock.
    out.trace_rank_variance = variance(per_rank_wall);

    if (!out.sketches_available) {
        std::fprintf(stderr,
                     "dlio: warning - AGGREGATION CF has no DDSketch data. "
                     "Distribution fitting will use mean-replication samples; "
                     "re-run dftracer_aggregator with --compute-percentiles "
                     "for higher-fidelity DLIO configs.\n");
    }

    return out;
}

BarrierSimulatorContext make_simulator_context(const AggregatedTraces& traces,
                                               int num_workers,
                                               int prefetch_factor) {
    BarrierSimulatorContext ctx;
    ctx.num_ranks = traces.num_ranks;
    ctx.num_steps = traces.num_steps;
    ctx.is_aggregated_trace = true;
    ctx.sync_mode = false;

    ctx.fetch_block_trace = traces.fetch_block_trace;
    ctx.fetch_iter_trace = traces.fetch_iter_trace;
    if (!traces.getitem_trace.empty()) {
        ctx.getitem_trace = traces.getitem_trace;
    }

    ctx.fetch_block_stats = traces.fetch_block_stats;
    ctx.fetch_iter_stats = traces.fetch_iter_stats;
    ctx.preprocess_stats = traces.preprocess_stats;
    ctx.getitem_stats = traces.getitem_stats;

    ctx.trace_fetch_block_metrics = traces.trace_fetch_block_metrics;
    ctx.trace_fetch_iter_metrics = traces.trace_fetch_iter_metrics;
    ctx.trace_preprocess_metrics = traces.trace_preprocess_metrics;

    ctx.trace_e2e_duration = traces.trace_e2e_duration;
    ctx.trace_rank_variance = traces.trace_rank_variance;
    ctx.trace_per_rank_throughput = traces.trace_per_rank_throughput;

    ctx.num_workers = num_workers;
    ctx.prefetch_factor = prefetch_factor;
    return ctx;
}

}  // namespace dftracer::utils::utilities::dlio
