#include <dftracer/utils/common/config.h>
#include <dftracer/utils/trace/comparator/comparison_result.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#endif

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::trace::comparator {

std::vector<MetricComparison> build_metadata_metrics(
    const TraceMetadata& baseline, const TraceMetadata& variant) {
    std::vector<MetricComparison> out;

    auto make = [](const std::string& name, double bval,
                   double vval) -> MetricComparison {
        MetricComparison mc;
        mc.metric_name = name;
        mc.baseline_value = bval;
        mc.variant_value = vval;
        mc.delta = vval - bval;
        mc.pct_change = bval == 0.0 ? (vval != 0.0 ? 100.0 : 0.0)
                                    : (vval - bval) / bval * 100.0;
        mc.significance = Significance::Negligible;
        mc.is_regression = false;
        return mc;
    };

    out.push_back(make("unique_files", static_cast<double>(baseline.file_count),
                       static_cast<double>(variant.file_count)));
    out.push_back(make("proc_files",
                       static_cast<double>(baseline.proc_file_count),
                       static_cast<double>(variant.proc_file_count)));
    out.push_back(make("processes", static_cast<double>(baseline.process_count),
                       static_cast<double>(variant.process_count)));
    out.push_back(make("threads", static_cast<double>(baseline.thread_count),
                       static_cast<double>(variant.thread_count)));
    out.push_back(
        make("time_pipeline", baseline.makespan_us, variant.makespan_us));
    out.push_back(
        make("time_io", baseline.total_io_time_us, variant.total_io_time_us));
    out.push_back(
        make("total_bytes", baseline.total_bytes, variant.total_bytes));

    return out;
}

double compute_cohens_d(const dataframe::FieldStat& base, std::uint64_t n_base,
                        const dataframe::FieldStat& var, std::uint64_t n_var) {
    if (n_base < 2 || n_var < 2) return 0.0;
    // `m2` is the central second moment, sum (x - mean)^2.
    // Population variance: Var = m2 / n.
    auto pop_var = [](const dataframe::FieldStat& ms, std::uint64_t n) {
        const double nd = static_cast<double>(n);
        const double central = ms.cm2;
        return (central > 0.0 ? central : 0.0) / nd;
    };
    double var_base = pop_var(base, n_base);
    double var_var = pop_var(var, n_var);
    double pooled = std::sqrt((var_base + var_var) / 2.0);
    if (pooled < 1e-15) return 0.0;
    return (var.mean() - base.mean()) / pooled;
}

Significance classify_significance(double d) {
    double abs_d = std::abs(d);
    if (abs_d > 0.8) return Significance::Large;
    if (abs_d > 0.5) return Significance::Medium;
    if (abs_d > 0.2) return Significance::Small;
    return Significance::Negligible;
}

namespace {

double safe_pct_change(double baseline, double variant) {
    if (std::isnan(baseline) || std::isnan(variant)) return 0.0;
    if (baseline == 0.0) return variant != 0.0 ? 100.0 : 0.0;
    return (variant - baseline) / baseline * 100.0;
}

double safe_quantile(const QuantileStore& store, double p) {
    if (store.count() == 0) return 0.0;
    double v = store.quantile(p);
    return std::isnan(v) ? 0.0 : v;
}

std::string percentile_label(const std::string& prefix, double p) {
    return prefix + "_p" + std::to_string(static_cast<int>(p * 100.0));
}

}  // anonymous namespace

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

            for (double p : percentiles) {
                MetricComparison pct_cmp;
                pct_cmp.metric_name = percentile_label("dur", p);
                pct_cmp.baseline_value = safe_quantile(bm.duration_q, p);
                pct_cmp.variant_value = safe_quantile(vm.duration_q, p);
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
                pct_cmp.baseline_value = safe_quantile(bm.size_q, p);
                pct_cmp.variant_value = safe_quantile(vm.size_q, p);
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

// Metric-name helpers are used by the tree formatter too, so keep them out of
// the Arrow ifdef below.

bool is_atomic_metric(const std::string& name) {
    return name == "count" || name == "transfer_size" || name == "bandwidth" ||
           name == "unique_files" || name == "proc_files" ||
           name == "processes" || name == "threads" || name == "total_bytes";
}

std::string metric_group(const std::string& name) {
    if (is_atomic_metric(name)) return "";
    auto pos = name.find('_');
    if (pos == std::string::npos) return "";
    return name.substr(0, pos);
}

std::string metric_leaf(const std::string& name) {
    if (is_atomic_metric(name)) return name;
    auto pos = name.find('_');
    if (pos == std::string::npos) return name;
    return name.substr(pos + 1);
}

const char* significance_to_string(Significance s) {
    switch (s) {
        case Significance::Negligible:
            return "NEGLIGIBLE";
        case Significance::Small:
            return "SMALL";
        case Significance::Medium:
            return "MEDIUM";
        case Significance::Large:
            return "LARGE";
    }
    return "NEGLIGIBLE";
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW
using utilities::common::arrow::ArrowExportResult;
using utilities::common::arrow::ColumnType;
using utilities::common::arrow::RecordBatchBuilder;

namespace {

void flatten_node(RecordBatchBuilder& builder, const NodeResult& node,
                  const std::string& parent_path) {
    std::string path =
        parent_path.empty() ? node.name : parent_path + "/" + node.name;

    auto emit = [&](const std::string& sub_path,
                    const std::vector<MetricComparison>& metrics) {
        for (const auto& mc : metrics) {
            if (mc.baseline_value == 0.0 && mc.variant_value == 0.0) continue;
            builder.append_string(0, sub_path);
            builder.append_string(1, metric_group(mc.metric_name));
            builder.append_string(2, metric_leaf(mc.metric_name));
            builder.append_double(3, mc.baseline_value);
            builder.append_double(4, mc.variant_value);
            builder.append_double(5, mc.baseline_stdev);
            builder.append_double(6, mc.variant_stdev);
            builder.append_double(7, mc.delta);
            builder.append_double(8, mc.pct_change);
            builder.append_double(9, mc.cohens_d);
            builder.append_string(10, significance_to_string(mc.significance));
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
        {"node_path", ColumnType::String},
        {"metric_group", ColumnType::String},
        {"metric_name", ColumnType::String},
        {"baseline", ColumnType::Double},
        {"variant", ColumnType::Double},
        {"baseline_stdev", ColumnType::Double},
        {"variant_stdev", ColumnType::Double},
        {"delta", ColumnType::Double},
        {"pct_change", ColumnType::Double},
        {"cohens_d", ColumnType::Double},
        {"significance", ColumnType::String},
        {"is_regression", ColumnType::Bool},
    });

    for (const auto& node : nodes) {
        flatten_node(builder, node, "");
    }

    return builder.finish();
}
#endif  // DFTRACER_UTILS_ENABLE_ARROW

}  // namespace dftracer::utils::trace::comparator
