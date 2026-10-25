#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/schemas/dft/agg/agg_partial.h>
#include <dftracer/utils/trace/agg_query.h>
#include <dftracer/utils/utilities/dlio/trace_loader.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace dftracer::utils::utilities::dlio {

namespace {

namespace agg = dftracer::utils::index::schemas::dft::agg;

constexpr double US_TO_S = 1e-6;

inline bool matches(std::string_view a, std::string_view b) { return a == b; }

struct ComponentAccumulator {
    // Per-(pid, time_bucket) entries used to materialize per-rank sample seqs.
    // Outer map sorts by pid for deterministic rank assignment; inner sorts by
    // time_bucket so the resulting sample sequence is in trace order.
    std::map<std::uint64_t, std::map<std::uint64_t, std::vector<double>>>
        per_pid_bucket_samples;
    // Boundary list (in microseconds) for sweep_union of trace-side wall clock.
    std::vector<Boundary> boundaries;
    // Merged duration quantiles across all entries; null until the first
    // entry with values is seen.
    dstd::SharedPtr<QuantileStore> quantiles;
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
// entry. When quantiles are available we draw `n` samples via inverse-CDF for
// within-bucket variance; otherwise we replicate the per-call mean. `n` is
// clamped to `max_samples` if non-zero.
void synthesize_samples(const agg::AggPartial& partial,
                        std::uint64_t max_samples, std::mt19937_64& rng,
                        std::vector<double>& out) {
    if (partial.count == 0) return;
    const auto desired =
        max_samples == 0 ? partial.count : std::min(partial.count, max_samples);
    if (desired == 0) return;

    const double mean_s = partial.duration.n == 0
                              ? 0.0
                              : partial.duration.sum /
                                    static_cast<double>(partial.duration.n) *
                                    US_TO_S;
    if (partial.duration_q.count() > 0) {
        std::uniform_real_distribution<double> u01(0.0, 1.0);
        out.reserve(out.size() + desired);
        for (std::uint64_t i = 0; i < desired; ++i) {
            const double q = u01(rng);
            out.push_back(partial.duration_q.quantile(q) * US_TO_S);
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
        for (const auto& bucket_kv : it->second) {
            const auto& samples = bucket_kv.second;
            rank_samples.insert(rank_samples.end(), samples.begin(),
                                samples.end());
        }
        out.push_back(std::move(rank_samples));
    }
    return out;
}

}  // namespace

AggregatedTraces load_aggregated_traces(const std::string& index_path,
                                        const dstd::Vector<dstd::String>& files,
                                        const agg::AggregationConfig& config,
                                        const TraceLoaderOptions& options) {
    AggregatedTraces out;
    out.time_interval_us = config.time_interval_us;

    const EventSelector* const selectors[] = {
        &options.fetch_block, &options.fetch_iter, &options.preprocess,
        &options.item};
    trace::AggQuery query;
    trace::AggQuerySource source;
    source.index_path = dstd::String(index_path);
    source.files = files;
    query.sources.push_back(std::move(source));
    query.config = config;
    query.by_pid = true;
    query.window_us = config.time_interval_us;
    query.quantiles = true;
    query.args = false;
    for (const auto* sel : selectors) {
        const dstd::String name(sel->name);
        if (std::find(query.names.begin(), query.names.end(), name) ==
            query.names.end())
            query.names.push_back(name);
    }

    auto queried = trace::run_agg_query(query).get();
    if (!queried) {
        throw DFTUtilsException(ErrorCode::Io,
                                "dlio: aggregation query failed: " +
                                    std::string(queried.error().format()));
    }
    trace::AggQueryResult& result = *queried;
    out.chunks_reread = result.stats.chunks_reread;

    ComponentAccumulator acc_fetch_block;
    ComponentAccumulator acc_fetch_iter;
    ComponentAccumulator acc_preprocess;
    ComponentAccumulator acc_getitem;

    std::unordered_set<std::uint64_t> pid_set;
    std::mt19937_64 rng(options.seed);

    // The row map has no order, and the sample draws share one generator, so
    // rows go by their key. Names, not ids: ids follow the order of interning.
    dstd::Vector<const trace::AggQueryResult::Rows::value_type*> rows;
    rows.reserve(result.rows.size());
    for (const auto& row : result.rows) rows.push_back(&row);
    std::sort(rows.begin(), rows.end(), [&](const auto* a, const auto* b) {
        const trace::AggKey& x = a->first;
        const trace::AggKey& y = b->first;
        if (x.pid != y.pid) return x.pid < y.pid;
        if (x.window != y.window) return x.window < y.window;
        const auto cx = result.text(x.cat);
        const auto cy = result.text(y.cat);
        if (cx != cy) return cx < cy;
        return result.text(x.name) < result.text(y.name);
    });

    for (const auto* row : rows) {
        const trace::AggKey& key = row->first;
        const agg::AggPartial& partial = row->second;
        const std::string_view cat = result.text(key.cat);
        const std::string_view name = result.text(key.name);

        ComponentAccumulator* target = nullptr;
        if (matches(cat, options.fetch_block.cat) &&
            matches(name, options.fetch_block.name)) {
            target = &acc_fetch_block;
        } else if (matches(cat, options.fetch_iter.cat) &&
                   matches(name, options.fetch_iter.name)) {
            target = &acc_fetch_iter;
        } else if (matches(cat, options.preprocess.cat) &&
                   matches(name, options.preprocess.name)) {
            target = &acc_preprocess;
        } else if (matches(cat, options.item.cat) &&
                   matches(name, options.item.name)) {
            target = &acc_getitem;
        } else {
            continue;
        }

        if (partial.count == 0) continue;

        out.any_data = true;
        pid_set.insert(key.pid);

        // Synthesize per-call samples for this entry.
        auto& bucket_vec = target->per_pid_bucket_samples[key.pid][key.window];
        synthesize_samples(partial, options.max_samples_per_entry, rng,
                           bucket_vec);

        // Accumulate component-level state.
        target->accumulated_time_s += partial.duration.sum * US_TO_S;
        target->total_count += partial.count;
        if (partial.duration.n > 0)
            apply_minmax(*target, partial.duration.min, partial.duration.max);

        // Real per-entry (ts, te) interval for trace-side union time.
        if (partial.te > partial.ts) {
            target->boundaries.push_back(
                {static_cast<std::int64_t>(partial.ts), +1});
            target->boundaries.push_back(
                {static_cast<std::int64_t>(partial.te), -1});
        }

        // Merge quantiles when present.
        if (partial.duration_q.count() > 0) {
            out.quantiles_available = true;
            if (!target->quantiles)
                target->quantiles =
                    dstd::make_shared<QuantileStore>(config.sketch_accuracy);
            target->quantiles->merge(partial.duration_q);
        }
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
    for (const auto& pid_kv : acc_preprocess.per_pid_bucket_samples) {
        for (const auto& bucket_kv : pid_kv.second) {
            const auto& samples = bucket_kv.second;
            out.preprocess_times.insert(out.preprocess_times.end(),
                                        samples.begin(), samples.end());
        }
    }

    // Quantiles + Statistic objects.
    auto attach_stat = [](Statistic& stat, const ComponentAccumulator& a) {
        if (!a.min_max_seen) return;
        // Seed Statistic with min and max (in seconds) so its fallback quantile
        // path has reasonable bounds even without quantiles.
        stat.update(a.min_us * US_TO_S);
        if (a.max_us != a.min_us) stat.update(a.max_us * US_TO_S);
        if (a.quantiles) {
            stat.attach_quantiles(a.quantiles);
        }
    };
    attach_stat(out.fetch_block_stats, acc_fetch_block);
    attach_stat(out.fetch_iter_stats, acc_fetch_iter);
    attach_stat(out.preprocess_stats, acc_preprocess);
    attach_stat(out.getitem_stats, acc_getitem);
    out.fetch_block_quantiles = acc_fetch_block.quantiles;
    out.fetch_iter_quantiles = acc_fetch_iter.quantiles;
    out.preprocess_quantiles = acc_preprocess.quantiles;
    out.getitem_quantiles = acc_getitem.quantiles;

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
            for (const auto& bucket_kv : pit->second) {
                count += bucket_kv.second.size();
            }
            // Wall clock per rank = sum of sample durations as an upper bound.
            for (const auto& bucket_kv : pit->second) {
                for (double s : bucket_kv.second) wall_us += s / US_TO_S;
            }
        }
        const double wall_s = wall_us * US_TO_S;
        per_rank_wall.push_back(wall_s);
        out.trace_per_rank_throughput.push_back(
            wall_s > 0.0 ? static_cast<double>(count) / wall_s : 0.0);
    }

    // Trace rank variance = variance of per-rank wall clock.
    out.trace_rank_variance = variance(per_rank_wall);

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

namespace {

void overlay_selector(const YAML::Node& root, const char* key,
                      EventSelector& sel) {
    const auto node = root[key];
    if (!node || !node.IsMap()) return;
    if (const auto cat = node["cat"]) sel.cat = cat.as<std::string>();
    if (const auto name = node["name"]) sel.name = name.as<std::string>();
}

}  // namespace

void load_event_map(const std::string& path, TraceLoaderOptions& options) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception& e) {
        throw DFTUtilsException(
            ErrorCode::InvalidArgument,
            "dlio: failed to parse event map " + path + ": " + e.what());
    }
    if (!root.IsMap()) {
        throw DFTUtilsException(ErrorCode::InvalidArgument,
                                "dlio: event map " + path +
                                    " must be a mapping of component names");
    }
    overlay_selector(root, "fetch_block", options.fetch_block);
    overlay_selector(root, "fetch_iter", options.fetch_iter);
    overlay_selector(root, "preprocess", options.preprocess);
    overlay_selector(root, "item", options.item);
}

}  // namespace dftracer::utils::utilities::dlio
