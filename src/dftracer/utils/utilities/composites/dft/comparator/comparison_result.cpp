#include <dftracer/utils/utilities/composites/dft/comparator/comparison_result.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#endif

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::comparator {

TraceMetadata extract_metadata(
    const std::unordered_map<AggregationKey, AggregationMetrics,
                             AggregationKeyHash>& aggregations,
    std::size_t file_count) {
    TraceMetadata meta;
    meta.file_count = file_count;

    std::unordered_set<std::uint64_t> pids;
    // Use pid+tid combined as unique thread identifier
    std::unordered_set<std::uint64_t> tids;
    std::uint64_t earliest_ts = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t latest_te = 0;

    for (const auto& [key, metrics] : aggregations) {
        pids.insert(key.pid);
        // Combine pid+tid for unique thread counting
        std::uint64_t ptid = (key.pid << 32) | (key.tid & 0xFFFFFFFF);
        tids.insert(ptid);

        meta.total_io_time_us += static_cast<double>(metrics.duration.total);

        if (internal::is_data_transfer_op(key.cat, key.name)) {
            meta.total_bytes += static_cast<double>(metrics.size.total);
        }

        if (metrics.ts < earliest_ts) earliest_ts = metrics.ts;
        if (metrics.te > latest_te) latest_te = metrics.te;
    }

    meta.process_count = pids.size();
    meta.thread_count = tids.size();
    if (latest_te > earliest_ts) {
        meta.makespan_us = static_cast<double>(latest_te - earliest_ts);
    }

    return meta;
}

double compute_cohens_d(const MetricStats& base, std::uint64_t n_base,
                        const MetricStats& var, std::uint64_t n_var) {
    if (n_base < 2 || n_var < 2) return 0.0;
    double var_base = base.m2 / static_cast<double>(n_base);
    double var_var = var.m2 / static_cast<double>(n_var);
    double pooled = std::sqrt((var_base + var_var) / 2.0);
    if (pooled < 1e-15) return 0.0;
    return (var.mean - base.mean) / pooled;
}

Significance classify_significance(double d) {
    double abs_d = std::abs(d);
    if (abs_d > 0.8) return Significance::LARGE;
    if (abs_d > 0.5) return Significance::MEDIUM;
    if (abs_d > 0.2) return Significance::SMALL;
    return Significance::NEGLIGIBLE;
}

namespace {

double safe_pct_change(double baseline, double variant) {
    if (std::isnan(baseline) || std::isnan(variant)) return 0.0;
    if (baseline == 0.0) return variant != 0.0 ? 100.0 : 0.0;
    return (variant - baseline) / baseline * 100.0;
}

double safe_quantile(const MetricStats& stats, double p) {
    double v = stats.sketch.quantile(p);
    return std::isnan(v) ? 0.0 : v;
}

std::string percentile_label(const std::string& prefix, double p) {
    return prefix + "_p" + std::to_string(static_cast<int>(p * 100.0));
}

// Key for (cat, name, time_bucket) — used to group within a window.
struct WindowKey {
    std::string cat;
    std::string name;
    std::uint64_t time_bucket;
    bool operator==(const WindowKey& o) const {
        return cat == o.cat && name == o.name && time_bucket == o.time_bucket;
    }
};

struct WindowKeyHash {
    std::size_t operator()(const WindowKey& k) const {
        std::size_t h = std::hash<std::string>{}(k.cat);
        h ^=
            std::hash<std::string>{}(k.name) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::uint64_t>{}(k.time_bucket) + 0x9e3779b9 + (h << 6) +
             (h >> 2);
        return h;
    }
};

// Per-window max across processes for a (cat, name, time_bucket).
struct WindowMax {
    double count = 0.0;
    double dur_mean = 0.0;
    double size_mean = 0.0;
    double xfer_size = 0.0;  // total_bytes / count
    double bandwidth = 0.0;  // total_bytes / total_dur_sec
    AggregationMetrics merged{0.01};
};

double safe_div(double a, double b) { return b > 0.0 ? a / b : 0.0; }

}  // anonymous namespace

CollapsedMap collapse_by_group(
    const std::unordered_map<AggregationKey, AggregationMetrics,
                             AggregationKeyHash>& aggregations) {
    // Step 1: For each (cat, name, time_bucket), take max across pids.
    std::unordered_map<WindowKey, WindowMax, WindowKeyHash> windows;

    for (const auto& [key, m] : aggregations) {
        WindowKey wk{key.cat, key.name, key.time_bucket};
        auto& w = windows[wk];

        double cnt = static_cast<double>(m.count);
        double dm = std::isnan(m.duration.mean) ? 0.0 : m.duration.mean;
        double sm = std::isnan(m.size.mean) ? 0.0 : m.size.mean;
        if (cnt > w.count) w.count = cnt;
        if (dm > w.dur_mean) w.dur_mean = dm;
        if (sm > w.size_mean) w.size_mean = sm;

        // Only compute transfer_size/bandwidth for actual I/O ops
        if (internal::is_data_transfer_op(key.cat, key.name)) {
            double total_bytes = static_cast<double>(m.size.total);
            double total_dur_us = static_cast<double>(m.duration.total);
            double xfer = safe_div(total_bytes, cnt);
            double bw = safe_div(total_bytes, total_dur_us / 1e6);
            if (xfer > w.xfer_size) w.xfer_size = xfer;
            if (bw > w.bandwidth) w.bandwidth = bw;
        }
        w.merged.merge_from(m);
    }

    // Step 2: Group by (cat, name), compute mean ± stdev across windows.
    struct GroupAccum {
        std::vector<double> counts;
        std::vector<double> dur_means;
        std::vector<double> size_means;
        std::vector<double> xfers;
        std::vector<double> bws;
        AggregationMetrics merged{0.01};
    };
    std::unordered_map<AggregationKey, GroupAccum, AggregationKeyHash> groups;

    for (auto& [wk, w] : windows) {
        AggregationKey gk;
        gk.cat = wk.cat;
        gk.name = wk.name;
        gk.pid = 0;
        gk.tid = 0;
        gk.time_bucket = 0;
        auto& g = groups[gk];
        g.counts.push_back(w.count);
        g.dur_means.push_back(w.dur_mean);
        g.size_means.push_back(w.size_mean);
        g.xfers.push_back(w.xfer_size);
        g.bws.push_back(w.bandwidth);
        g.merged.merge_from(w.merged);
    }

    // Helper: compute mean and stdev of a vector.
    auto mean_stdev =
        [](const std::vector<double>& v) -> std::pair<double, double> {
        if (v.empty()) return {0.0, 0.0};
        double sum = 0.0;
        for (double x : v) sum += x;
        double mean = sum / static_cast<double>(v.size());
        if (v.size() < 2) return {mean, 0.0};
        double sq = 0.0;
        for (double x : v) {
            double d = x - mean;
            sq += d * d;
        }
        return {mean, std::sqrt(sq / (static_cast<double>(v.size()) - 1.0))};
    };

    CollapsedMap result;
    for (auto& [gk, g] : groups) {
        CollapsedMetrics cm;
        cm.merged = std::move(g.merged);
        cm.num_windows = g.dur_means.size();

        auto [cm_mean, cm_std] = mean_stdev(g.counts);
        cm.count_mean = cm_mean;

        auto [dm, ds] = mean_stdev(g.dur_means);
        cm.dur_mean_of_means = dm;
        cm.dur_stdev_of_means = ds;

        auto [sm, ss] = mean_stdev(g.size_means);
        cm.size_mean_of_means = sm;
        cm.size_stdev_of_means = ss;

        auto [xm, xs] = mean_stdev(g.xfers);
        cm.xfer_mean = xm;
        cm.xfer_stdev = xs;

        auto [bm, bs] = mean_stdev(g.bws);
        cm.bw_mean = bm;
        cm.bw_stdev = bs;

        result[gk] = std::move(cm);
    }
    return result;
}

std::vector<MetricComparison> compare_metrics(
    const CollapsedMetrics& baseline, const CollapsedMetrics& variant,
    const std::vector<std::string>& metrics,
    const std::vector<double>& percentiles) {
    std::vector<MetricComparison> result;
    const auto& bm = baseline.merged;
    const auto& vm = variant.merged;

    for (const auto& metric : metrics) {
        if (metric == "count") {
            MetricComparison cmp;
            cmp.metric_name = "count";
            cmp.baseline_value = static_cast<double>(bm.count);
            cmp.variant_value = static_cast<double>(vm.count);
            cmp.delta = cmp.variant_value - cmp.baseline_value;
            cmp.pct_change =
                safe_pct_change(cmp.baseline_value, cmp.variant_value);
            cmp.is_regression = cmp.delta > 0.0;
            result.push_back(std::move(cmp));
        } else if (metric == "duration") {
            // mean entry: use mean-of-means
            MetricComparison mean_cmp;
            mean_cmp.metric_name = "dur_mean";
            mean_cmp.baseline_value = baseline.dur_mean_of_means;
            mean_cmp.variant_value = variant.dur_mean_of_means;
            mean_cmp.baseline_stdev = baseline.dur_stdev_of_means;
            mean_cmp.variant_stdev = variant.dur_stdev_of_means;
            mean_cmp.delta = mean_cmp.variant_value - mean_cmp.baseline_value;
            mean_cmp.pct_change = safe_pct_change(mean_cmp.baseline_value,
                                                  mean_cmp.variant_value);
            mean_cmp.cohens_d =
                compute_cohens_d(bm.duration, bm.count, vm.duration, vm.count);
            mean_cmp.significance = classify_significance(mean_cmp.cohens_d);
            mean_cmp.is_regression = mean_cmp.delta > 0.0;
            result.push_back(std::move(mean_cmp));

            // percentile entries from merged DDSketch
            for (double p : percentiles) {
                MetricComparison pct_cmp;
                pct_cmp.metric_name = percentile_label("dur", p);
                pct_cmp.baseline_value = safe_quantile(bm.duration, p);
                pct_cmp.variant_value = safe_quantile(vm.duration, p);
                pct_cmp.delta = pct_cmp.variant_value - pct_cmp.baseline_value;
                pct_cmp.pct_change = safe_pct_change(pct_cmp.baseline_value,
                                                     pct_cmp.variant_value);
                pct_cmp.is_regression = pct_cmp.delta > 0.0;
                result.push_back(std::move(pct_cmp));
            }
        } else if (metric == "size") {
            MetricComparison mean_cmp;
            mean_cmp.metric_name = "size_mean";
            mean_cmp.baseline_value = baseline.size_mean_of_means;
            mean_cmp.variant_value = variant.size_mean_of_means;
            mean_cmp.baseline_stdev = baseline.size_stdev_of_means;
            mean_cmp.variant_stdev = variant.size_stdev_of_means;
            mean_cmp.delta = mean_cmp.variant_value - mean_cmp.baseline_value;
            mean_cmp.pct_change = safe_pct_change(mean_cmp.baseline_value,
                                                  mean_cmp.variant_value);
            mean_cmp.cohens_d =
                compute_cohens_d(bm.size, bm.count, vm.size, vm.count);
            mean_cmp.significance = classify_significance(mean_cmp.cohens_d);
            mean_cmp.is_regression = mean_cmp.delta > 0.0;
            result.push_back(std::move(mean_cmp));

            for (double p : percentiles) {
                MetricComparison pct_cmp;
                pct_cmp.metric_name = percentile_label("size", p);
                pct_cmp.baseline_value = safe_quantile(bm.size, p);
                pct_cmp.variant_value = safe_quantile(vm.size, p);
                pct_cmp.delta = pct_cmp.variant_value - pct_cmp.baseline_value;
                pct_cmp.pct_change = safe_pct_change(pct_cmp.baseline_value,
                                                     pct_cmp.variant_value);
                pct_cmp.is_regression = pct_cmp.delta > 0.0;
                result.push_back(std::move(pct_cmp));
            }
        } else if (metric == "transfer_size") {
            double bv = baseline.xfer_mean;
            double vv = variant.xfer_mean;
            if (bv > 0.0 || vv > 0.0) {
                MetricComparison cmp;
                cmp.metric_name = "transfer_size";
                cmp.baseline_value = bv;
                cmp.variant_value = vv;
                cmp.baseline_stdev = baseline.xfer_stdev;
                cmp.variant_stdev = variant.xfer_stdev;
                cmp.delta = vv - bv;
                cmp.pct_change = safe_pct_change(bv, vv);
                cmp.is_regression = cmp.delta > 0.0;
                result.push_back(std::move(cmp));
            }
        } else if (metric == "bandwidth") {
            double bv = baseline.bw_mean;
            double vv = variant.bw_mean;
            if (bv > 0.0 || vv > 0.0) {
                MetricComparison cmp;
                cmp.metric_name = "bandwidth";
                cmp.baseline_value = bv;
                cmp.variant_value = vv;
                cmp.baseline_stdev = baseline.bw_stdev;
                cmp.variant_stdev = variant.bw_stdev;
                cmp.delta = vv - bv;
                cmp.pct_change = safe_pct_change(bv, vv);
                cmp.is_regression = cmp.delta < 0.0;
                result.push_back(std::move(cmp));
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// ComparisonOutput::to_arrow
// ---------------------------------------------------------------------------

#ifdef DFTRACER_UTILS_ENABLE_ARROW
using common::arrow::ArrowExportResult;
using common::arrow::ColumnType;
using common::arrow::RecordBatchBuilder;

namespace {

const char* sig_to_str(Significance s) {
    switch (s) {
        case Significance::NEGLIGIBLE:
            return "NEGLIGIBLE";
        case Significance::SMALL:
            return "SMALL";
        case Significance::MEDIUM:
            return "MEDIUM";
        case Significance::LARGE:
            return "LARGE";
    }
    return "NEGLIGIBLE";
}

// Metric names that are atomic (not group_prefix + leaf).
bool is_atomic_metric(const std::string& name) {
    return name == "count" || name == "transfer_size" || name == "bandwidth";
}

// Extract metric group prefix: "dur_mean" -> "dur", "count" -> ""
std::string arrow_metric_group(const std::string& name) {
    if (is_atomic_metric(name)) return "";
    auto pos = name.find('_');
    if (pos == std::string::npos) return "";
    return name.substr(0, pos);
}

// Strip prefix: "dur_mean" -> "mean", "count" -> "count"
std::string arrow_metric_leaf(const std::string& name) {
    if (is_atomic_metric(name)) return name;
    auto pos = name.find('_');
    if (pos == std::string::npos) return name;
    return name.substr(pos + 1);
}

void flatten_node(RecordBatchBuilder& builder, const NodeResult& node,
                  const std::string& parent_path) {
    std::string path =
        parent_path.empty() ? node.name : parent_path + "/" + node.name;

    // Summary metrics
    auto emit = [&](const std::string& sub_path,
                    const std::vector<MetricComparison>& metrics) {
        for (const auto& mc : metrics) {
            if (mc.baseline_value == 0.0 && mc.variant_value == 0.0) continue;
            builder.append_string(0, sub_path);
            builder.append_string(1, arrow_metric_group(mc.metric_name));
            builder.append_string(2, arrow_metric_leaf(mc.metric_name));
            builder.append_double(3, mc.baseline_value);
            builder.append_double(4, mc.variant_value);
            builder.append_double(5, mc.baseline_stdev);
            builder.append_double(6, mc.variant_stdev);
            builder.append_double(7, mc.delta);
            builder.append_double(8, mc.pct_change);
            builder.append_double(9, mc.cohens_d);
            builder.append_string(10, sig_to_str(mc.significance));
            builder.append_bool(11, mc.is_regression);
            builder.end_row();
        }
    };

    emit(path + "/SUMMARY", node.summary.metrics);

    for (const auto& g : node.groups) {
        emit(path + "/" + g.label, g.metrics);
    }

    for (const auto& child : node.children) {
        flatten_node(builder, child, path);
    }
}

}  // anonymous namespace

ArrowExportResult ComparisonOutput::to_arrow() const {
    RecordBatchBuilder builder;
    builder.declare_schema({
        {"node_path", ColumnType::STRING},
        {"metric_group", ColumnType::STRING},
        {"metric_name", ColumnType::STRING},
        {"baseline", ColumnType::DOUBLE},
        {"variant", ColumnType::DOUBLE},
        {"baseline_stdev", ColumnType::DOUBLE},
        {"variant_stdev", ColumnType::DOUBLE},
        {"delta", ColumnType::DOUBLE},
        {"pct_change", ColumnType::DOUBLE},
        {"cohens_d", ColumnType::DOUBLE},
        {"significance", ColumnType::STRING},
        {"is_regression", ColumnType::BOOL},
    });

    for (const auto& node : nodes) {
        flatten_node(builder, node, "");
    }

    return builder.finish();
}
#endif  // DFTRACER_UTILS_ENABLE_ARROW

}  // namespace dftracer::utils::utilities::composites::dft::comparator
