#include <dftracer/utils/utilities/common/serialization/binary_codec.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/system_metrics_serialization.h>

#include <cstring>

namespace dftracer::utils::utilities::composites::dft::aggregators {

namespace {

using common::serialization::BinaryReader;
using common::serialization::put_blob;
using common::serialization::put_double;
using common::serialization::put_str;
using common::serialization::put_varint;

void serialize_float_metric_stats(std::string& out,
                                  const FloatMetricStats& ms) {
    put_varint(out, ms.count);
    put_double(out, ms.total);
    put_double(out, ms.min);
    put_double(out, ms.max);
    put_double(out, ms.mean);
    put_double(out, ms.m2);
    // m3/m4 not persisted yet -- skewness/kurtosis are recomputed
    // in-memory only.
    // put_double(out, ms.m3);
    // put_double(out, ms.m4);
    if (ms.sketch) {
        out.push_back(1);
        auto blob = ms.sketch->serialize();
        put_blob(out, blob);
    } else {
        out.push_back(0);
    }
}

FloatMetricStats deserialize_float_metric_stats(BinaryReader& r,
                                                double accuracy) {
    FloatMetricStats ms(accuracy);
    ms.count = r.varint();
    ms.total = r.f64();
    ms.min = r.f64();
    ms.max = r.f64();
    ms.mean = r.f64();
    ms.m2 = r.f64();
    // ms.m3 = r.f64();
    // ms.m4 = r.f64();
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
    out.reserve(4 + hhash.size() + name.size() + 10);
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
        m.metrics = std::make_unique<FloatMetricsMap>();
        for (std::uint32_t i = 0; i < num_metrics; ++i) {
            auto name = r.str();
            auto stats = deserialize_float_metric_stats(r, m.sketch_accuracy);
            m.metrics->emplace(std::string(name), std::move(stats));
        }
    }
    return m;
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
