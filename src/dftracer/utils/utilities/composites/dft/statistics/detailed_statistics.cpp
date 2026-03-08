#include <dftracer/utils/utilities/composites/dft/statistics/detailed_statistics.h>
#include <yyjson.h>

#include <cmath>
#include <cstdint>

namespace dftracer::utils::utilities::composites::dft::statistics {

// --- DistributionStats ---

void DistributionStats::update(double value) {
    histogram.add(static_cast<std::uint64_t>(value));
    sketch.add(value);
    sum += value;
    sum_sq += value * value;
}

void DistributionStats::merge(const DistributionStats& other) {
    histogram.merge(other.histogram);
    sketch.merge(other.sketch);
    sum += other.sum;
    sum_sq += other.sum_sq;
}

std::uint64_t DistributionStats::count() const {
    return histogram.total_count();
}

double DistributionStats::mean() const {
    if (count() == 0) return 0.0;
    return sum / static_cast<double>(count());
}

double DistributionStats::stddev() const {
    auto n = count();
    if (n < 2) return 0.0;
    double dn = static_cast<double>(n);
    double m = mean();
    double variance = (sum_sq - dn * m * m) / (dn - 1.0);
    return variance > 0.0 ? std::sqrt(variance) : 0.0;
}

// --- IOEventMetrics ---

void IOEventMetrics::merge(const IOEventMetrics& other) {
    duration.merge(other.duration);
    size.merge(other.size);
    bandwidth.merge(other.bandwidth);
    offset.merge(other.offset);
}

// --- DetailedStatistics ---

void DetailedStatistics::merge(const DetailedStatistics& other) {
    duration.merge(other.duration);

    for (const auto& [key, dist] : other.grouped_duration) {
        grouped_duration[key].merge(dist);
    }

    for (const auto& [key, io] : other.grouped_io) {
        grouped_io[key].merge(io);
    }

    for (const auto& [key, cat] : other.group_key_category) {
        group_key_category.emplace(key, cat);
    }

    events_scanned += other.events_scanned;
    chunks_scanned += other.chunks_scanned;
    chunks_skipped += other.chunks_skipped;
}

// Helper: serialize a DistributionStats into a yyjson mutable object
static yyjson_mut_val* distribution_to_json(yyjson_mut_doc* doc,
                                            const DistributionStats& dist) {
    yyjson_mut_val* obj = yyjson_mut_obj(doc);

    yyjson_mut_obj_add_uint(doc, obj, "count", dist.count());
    yyjson_mut_obj_add_real(doc, obj, "sum", dist.sum);
    yyjson_mut_obj_add_real(doc, obj, "mean", dist.mean());
    yyjson_mut_obj_add_real(doc, obj, "stddev", dist.stddev());

    if (dist.count() > 0 && !dist.sketch.empty()) {
        yyjson_mut_obj_add_real(doc, obj, "min", dist.sketch.min());
        yyjson_mut_obj_add_real(doc, obj, "max", dist.sketch.max());

        yyjson_mut_val* pctls = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_real(doc, pctls, "p10", dist.sketch.quantile(0.1));
        yyjson_mut_obj_add_real(doc, pctls, "p25", dist.sketch.quantile(0.25));
        yyjson_mut_obj_add_real(doc, pctls, "p50", dist.sketch.quantile(0.5));
        yyjson_mut_obj_add_real(doc, pctls, "p75", dist.sketch.quantile(0.75));
        yyjson_mut_obj_add_real(doc, pctls, "p90", dist.sketch.quantile(0.9));
        yyjson_mut_obj_add_real(doc, pctls, "p95", dist.sketch.quantile(0.95));
        yyjson_mut_obj_add_real(doc, pctls, "p99", dist.sketch.quantile(0.99));
        yyjson_mut_obj_add_val(doc, obj, "percentiles", pctls);
    }

    // Direct serialization to avoid string roundtrip
    yyjson_mut_val* hist_val = dist.histogram.to_yyjson(doc);
    yyjson_mut_obj_add_val(doc, obj, "histogram", hist_val);

    return obj;
}

// Helper: serialize IOEventMetrics into a yyjson mutable object
static yyjson_mut_val* io_metrics_to_json(yyjson_mut_doc* doc,
                                          const IOEventMetrics& io) {
    yyjson_mut_val* obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, obj, "duration",
                           distribution_to_json(doc, io.duration));
    yyjson_mut_obj_add_val(doc, obj, "size",
                           distribution_to_json(doc, io.size));
    if (io.bandwidth.count() > 0) {
        yyjson_mut_obj_add_val(doc, obj, "bandwidth",
                               distribution_to_json(doc, io.bandwidth));
    }
    if (io.offset.count() > 0) {
        yyjson_mut_obj_add_val(doc, obj, "offset",
                               distribution_to_json(doc, io.offset));
    }
    return obj;
}

std::string DetailedStatistics::to_json() const {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    // Scan progress
    yyjson_mut_obj_add_uint(doc, root, "events_scanned", events_scanned);
    yyjson_mut_obj_add_uint(doc, root, "chunks_scanned", chunks_scanned);
    yyjson_mut_obj_add_uint(doc, root, "chunks_skipped", chunks_skipped);

    // Global duration
    yyjson_mut_obj_add_val(doc, root, "duration",
                           distribution_to_json(doc, duration));

    // Grouped duration
    if (!grouped_duration.empty()) {
        yyjson_mut_val* gd = yyjson_mut_obj(doc);
        for (const auto& [key, dist] : grouped_duration) {
            yyjson_mut_obj_add_val(doc, gd, key.c_str(),
                                   distribution_to_json(doc, dist));
        }
        yyjson_mut_obj_add_val(doc, root, "grouped_duration", gd);
    }

    // Grouped I/O
    if (!grouped_io.empty()) {
        yyjson_mut_val* gio = yyjson_mut_obj(doc);
        for (const auto& [key, io] : grouped_io) {
            yyjson_mut_obj_add_val(doc, gio, key.c_str(),
                                   io_metrics_to_json(doc, io));
        }
        yyjson_mut_obj_add_val(doc, root, "grouped_io", gio);
    }

    char* json_str = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, nullptr);
    std::string result(json_str ? json_str : "{}");
    if (json_str) free(json_str);
    yyjson_mut_doc_free(doc);
    return result;
}

}  // namespace dftracer::utils::utilities::composites::dft::statistics
