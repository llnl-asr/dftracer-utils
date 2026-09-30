#include <dftracer/utils/index/schemas/dft/agg/aggregation_serialization.h>
#include <dftracer/utils/index/schemas/dft/agg/system_metrics_serialization.h>
#include <dftracer/utils/utilities/common/serialization/binary_codec.h>
#include <dftracer/utils/utilities/hash/fnv1a_hasher_utility.h>

#include <cstring>

namespace dftracer::utils::index::schemas::dft::agg {

namespace {

using utilities::common::serialization::BinaryReader;
using utilities::common::serialization::put_be16;
using utilities::common::serialization::put_blob;
using utilities::common::serialization::put_double;
using utilities::common::serialization::put_str;
using utilities::common::serialization::put_varint;

// Shard prefix so the SYSTEM_METRICS CF can be range-split like the AGG CF; a
// host/metric's buckets share one shard.
std::uint16_t system_shard(std::string_view hhash, std::string_view name) {
    utilities::hash::Fnv1aHashBuilder h;
    h.update(hhash);
    h.update(name);
    return static_cast<std::uint16_t>(h.finish() % AGG_KEY_NUM_SHARDS);
}

// System metric values are fractional (CPU/GPU %), so sum/min/max and the
// moments ride as doubles rather than the varints the event codec uses. The
// moments are the running mean as the pair shift + cmean and the central second
// moment (the sum of (x - mean)^2), from which the stddev derives.
void serialize_float_metric_stats(std::string& out, const MetricStats& ms) {
    put_varint(out, ms.count());
    put_double(out, ms.total());
    put_double(out, ms.min());
    put_double(out, ms.max());
    put_double(out, ms.shift());
    put_double(out, ms.cmean());
    put_double(out, ms.m2());
    if (ms.sketch) {
        out.push_back(1);
        auto blob = ms.sketch->serialize();
        put_blob(out, blob);
    } else {
        out.push_back(0);
    }
}

MetricStats deserialize_float_metric_stats(BinaryReader& r, double accuracy) {
    MetricStats ms(accuracy);
    ms.stat.n = r.varint();
    ms.stat.sum = r.f64();
    ms.stat.min = r.f64();
    ms.stat.max = r.f64();
    ms.stat.shift = r.f64();
    ms.stat.cmean = r.f64();
    ms.stat.cm2 = r.f64();
    if (r.u8()) {
        auto blob = r.blob();
        ms.sketch = std::make_unique<DDSketch>(DDSketch::deserialize(
            reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size()));
    }
    return ms;
}

}  // namespace

void serialize_system_key_into(std::string& out, std::string_view hhash,
                               std::string_view name,
                               std::uint64_t time_bucket) {
    out.clear();
    out.reserve(6 + hhash.size() + name.size() + 10);
    put_be16(out, system_shard(hhash, name));
    put_str(out, hhash);
    put_str(out, name);
    put_varint(out, time_bucket);
}

std::string serialize_system_key(std::string_view hhash, std::string_view name,
                                 std::uint64_t time_bucket) {
    std::string out;
    serialize_system_key_into(out, hhash, name, time_bucket);
    return out;
}

DeserializedSystemKey deserialize_system_key(std::string_view data) {
    BinaryReader r(data);
    r.be16();  // shard prefix
    auto hhash = r.str();
    auto name = r.str();
    auto time_bucket = r.varint();
    return {{std::string(hhash), std::string(name), time_bucket}};
}

void serialize_system_value_into(std::string& out,
                                 const SystemAggregationMetrics& m) {
    out.clear();
    out.reserve(128);

    put_varint(out, m.count);
    put_varint(out, m.ts);
    put_varint(out, m.te);

    std::uint32_t num_metrics =
        m.metrics ? static_cast<std::uint32_t>(m.metrics->size()) : 0;
    put_varint(out, num_metrics);

    if (m.metrics) {
        for (const auto& [name, stats] : *m.metrics) {
            put_str(out, name);
            serialize_float_metric_stats(out, stats);
        }
    }
}

std::string serialize_system_value(const SystemAggregationMetrics& m) {
    std::string out;
    serialize_system_value_into(out, m);
    return out;
}

SystemAggregationMetrics deserialize_system_value(std::string_view data) {
    BinaryReader r(data);
    SystemAggregationMetrics m;
    m.count = r.varint();
    m.ts = r.varint();
    m.te = r.varint();

    auto num_metrics = r.varint();
    if (num_metrics > 0) {
        m.metrics = std::make_unique<SystemMetricsMap>();
        for (std::uint32_t i = 0; i < num_metrics; ++i) {
            auto name = r.str();
            auto stats = deserialize_float_metric_stats(r, m.sketch_accuracy);
            m.metrics->emplace(std::string(name), std::move(stats));
        }
    }
    return m;
}

}  // namespace dftracer::utils::index::schemas::dft::agg
