// SPDX-License-Identifier: MIT
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/statistics/detailed_statistics.h>

#include <cmath>
#include <cstdint>

namespace dftracer::utils::trace::statistics {

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

namespace {
void write_distribution_json(dstd::String& ss, const DistributionStats& dist) {
    ss += "{\"count\":";
    internal::append_value(ss, dist.count());
    ss += ",\"sum\":";
    internal::append_value(ss, dist.sum);
    ss += ",\"mean\":";
    internal::append_value(ss, dist.mean());
    ss += ",\"stddev\":";
    internal::append_value(ss, dist.stddev());

    if (dist.count() > 0 && !dist.sketch.empty()) {
        ss += ",\"min\":";
        internal::append_value(ss, dist.sketch.min());
        ss += ",\"max\":";
        internal::append_value(ss, dist.sketch.max());
        ss += ",\"percentiles\":{";
        ss += "\"p10\":";
        internal::append_value(ss, dist.sketch.quantile(0.1));
        ss += ",\"p25\":";
        internal::append_value(ss, dist.sketch.quantile(0.25));
        ss += ",\"p50\":";
        internal::append_value(ss, dist.sketch.quantile(0.5));
        ss += ",\"p75\":";
        internal::append_value(ss, dist.sketch.quantile(0.75));
        ss += ",\"p90\":";
        internal::append_value(ss, dist.sketch.quantile(0.9));
        ss += ",\"p95\":";
        internal::append_value(ss, dist.sketch.quantile(0.95));
        ss += ",\"p99\":";
        internal::append_value(ss, dist.sketch.quantile(0.99));
        ss += '}';
    }

    ss += ",\"histogram\":";
    internal::append_value(ss, dist.histogram.to_json_detailed());
    ss += '}';
}

void write_io_metrics_json(dstd::String& ss, const IOEventMetrics& io) {
    ss += "{\"duration\":";
    write_distribution_json(ss, io.duration);
    ss += ",\"size\":";
    write_distribution_json(ss, io.size);
    if (io.bandwidth.count() > 0) {
        ss += ",\"bandwidth\":";
        write_distribution_json(ss, io.bandwidth);
    }
    if (io.offset.count() > 0) {
        ss += ",\"offset\":";
        write_distribution_json(ss, io.offset);
    }
    ss += '}';
}
}  // namespace

dstd::String DetailedStatistics::to_json() const {
    dstd::String ss;
    ss += '{';

    ss += "\"events_scanned\":";
    internal::append_value(ss, events_scanned);
    ss += ",\"chunks_scanned\":";
    internal::append_value(ss, chunks_scanned);
    ss += ",\"chunks_skipped\":";
    internal::append_value(ss, chunks_skipped);

    ss += ",\"duration\":";
    write_distribution_json(ss, duration);

    if (!grouped_duration.empty()) {
        ss += ",\"grouped_duration\":{";
        bool first = true;
        for (const auto& [key, dist] : grouped_duration) {
            if (!first) ss += ',';
            first = false;
            ss += '"';
            internal::append_value(ss, key);
            ss += "\":";
            write_distribution_json(ss, dist);
        }
        ss += '}';
    }

    if (!grouped_io.empty()) {
        ss += ",\"grouped_io\":{";
        bool first = true;
        for (const auto& [key, io] : grouped_io) {
            if (!first) ss += ',';
            first = false;
            ss += '"';
            internal::append_value(ss, key);
            ss += "\":";
            write_io_metrics_json(ss, io);
        }
        ss += '}';
    }

    ss += '}';
    return ss;
}

}  // namespace dftracer::utils::trace::statistics
