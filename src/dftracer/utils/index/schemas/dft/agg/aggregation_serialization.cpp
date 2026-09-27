#include <dftracer/utils/core/common/hash/hex64.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_serialization.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/utilities/common/serialization/binary_codec.h>
#include <dftracer/utils/utilities/hash/fnv1a_hasher_utility.h>

#include <algorithm>
#include <cstring>

namespace dftracer::utils::index::schemas::dft::agg {

namespace {

namespace hash = dftracer::utils::utilities::hash;

using utilities::common::serialization::BinaryReader;
using utilities::common::serialization::put_be16;
using utilities::common::serialization::put_be64;
using utilities::common::serialization::put_blob;
using utilities::common::serialization::put_double;
using utilities::common::serialization::put_str;
using utilities::common::serialization::put_u8;
using utilities::common::serialization::put_varint;
using utilities::common::serialization::write_double;
using utilities::common::serialization::write_str;
using utilities::common::serialization::write_varint;

std::uint16_t compute_shard(std::string_view cat, std::string_view name,
                            std::uint64_t pid, std::uint64_t tid) {
    struct Cache {
        char cat_buf[64];
        char name_buf[64];
        std::size_t cat_len = SIZE_MAX;
        std::size_t name_len = SIZE_MAX;
        std::uint64_t pid = 0;
        std::uint64_t tid = 0;
        std::uint16_t shard = 0;
    };
    thread_local Cache cache;

    if (cat.size() == cache.cat_len && name.size() == cache.name_len &&
        pid == cache.pid && tid == cache.tid &&
        std::memcmp(cache.cat_buf, cat.data(), cat.size()) == 0 &&
        std::memcmp(cache.name_buf, name.data(), name.size()) == 0) {
        return cache.shard;
    }

    utilities::hash::Fnv1aHashBuilder h;
    h.update(cat);
    h.update(name);
    h.update_value(pid);
    h.update_value(tid);
    const auto shard =
        static_cast<std::uint16_t>(h.finish() % AGG_KEY_NUM_SHARDS);

    if (cat.size() <= sizeof(cache.cat_buf) &&
        name.size() <= sizeof(cache.name_buf)) {
        std::memcpy(cache.cat_buf, cat.data(), cat.size());
        std::memcpy(cache.name_buf, name.data(), name.size());
        cache.cat_len = cat.size();
        cache.name_len = name.size();
        cache.pid = pid;
        cache.tid = tid;
        cache.shard = shard;
    } else {
        cache.cat_len = SIZE_MAX;
    }
    return shard;
}

// Wire layout (FULL / FULL_WITH_SKETCH):
//   fmt:u8, count:varint, total:varint, min:varint, max:varint,
//   mean:f64, m2:f64, m3:f64, m4:f64, [sketch blob]
// m2/m3/m4 are raw power sums (sum_x^2/3/4); mean is redundantly persisted
// so consumers that don't need stddev can skip the power sums. m3/m4 let the
// tier answer skewness/kurtosis without a scan.
inline char* write_metric_stats(char* p, const MetricStats& ms) {
    // COMPACT format can only represent "empty" (count=0) or a single
    // event with value = total (count=1). Critically: count=1 total=0 is
    // a VALID state (one event with value 0) that COMPACT cannot round-
    // trip because the deserializer falls back to count=0 whenever the
    // serialized varint is 0. Avoid COMPACT for that case.
    // total/min/max are integer-valued (event metrics come from uint64), so the
    // wire keeps its varint encoding: cast the FieldStat doubles back to u64.
    const auto u = [](double d) { return static_cast<std::uint64_t>(d); };
    // Persist the exact integer accumulator when the metric stayed integral, so
    // a sum/min/max past 2^53 is not rounded through the double.
    const std::uint64_t xtotal =
        ms.exact_u64() ? ms.total_u64() : u(ms.total());
    const std::uint64_t xmin = ms.exact_u64() ? ms.min_u64() : u(ms.min());
    const std::uint64_t xmax = ms.exact_u64() ? ms.max_u64() : u(ms.max());
    const bool compact_empty =
        ms.count() == 0 && ms.total() == 0 && ms.m2() == 0.0 && !ms.sketch;
    const bool compact_single = ms.count() == 1 && ms.total() > 0 &&
                                ms.m2() == ms.total() * ms.total() &&
                                !ms.sketch;
    if (compact_empty || compact_single) {
        *p++ = static_cast<char>(METRIC_FMT_COMPACT);
        return write_varint(p, ms.count() == 0 ? 0 : xtotal);
    }
    *p++ = static_cast<char>(METRIC_FMT_FULL);
    p = write_varint(p, ms.count());
    p = write_varint(p, xtotal);
    p = write_varint(p, xmin);
    p = write_varint(p, xmax);
    p = write_double(p, ms.mean());
    p = write_double(p, ms.m2());
    p = write_double(p, ms.m3());
    p = write_double(p, ms.m4());
    return p;
}

// Upper bound for MetricStats (FULL fmt, no sketch):
//   1 (fmt) + 4*10 (varints) + 4*8 (doubles) = 73 bytes
constexpr std::size_t METRIC_STATS_MAX_BYTES_NO_SKETCH = 73;

void serialize_metric_stats(std::string& out, const MetricStats& ms) {
    if (!ms.sketch) {
        const auto old_size = out.size();
        out.resize(old_size + METRIC_STATS_MAX_BYTES_NO_SKETCH);
        char* begin = out.data() + old_size;
        char* p = write_metric_stats(begin, ms);
        out.resize(old_size + static_cast<std::size_t>(p - begin));
        return;
    }
    const auto u = [](double d) { return static_cast<std::uint64_t>(d); };
    put_u8(out, METRIC_FMT_FULL_WITH_SKETCH);
    put_varint(out, ms.count());
    put_varint(out, ms.exact_u64() ? ms.total_u64() : u(ms.total()));
    put_varint(out, ms.exact_u64() ? ms.min_u64() : u(ms.min()));
    put_varint(out, ms.exact_u64() ? ms.max_u64() : u(ms.max()));
    put_double(out, ms.mean());
    put_double(out, ms.m2());
    put_double(out, ms.m3());
    put_double(out, ms.m4());
    auto blob = ms.sketch->serialize();
    put_blob(out, blob);
}

MetricStats deserialize_metric_stats(BinaryReader& r, double accuracy) {
    auto fmt = r.u8();
    if (fmt == METRIC_FMT_COMPACT) {
        MetricStats ms(accuracy);
        auto val = r.varint();
        if (val > 0) {
            const double v = static_cast<double>(val);
            ms.stat.n = 1;
            ms.stat.sum = v;
            ms.stat.min = ms.stat.max = v;
            ms.stat.sumsq = v * v;
            ms.stat.m3 = v * v * v;
            ms.stat.m4 = v * v * v * v;
            // Tier metrics are uint64; restore the exact domain so a
            // tier-answered query matches the scan path.
            ms.stat.domain = dftracer::utils::dataframe::FieldStatDomain::U64;
            ms.stat.esum = ms.stat.emin = ms.stat.emax =
                std::bit_cast<std::int64_t>(val);
        }
        return ms;
    }
    MetricStats ms(accuracy);
    ms.stat.n = r.varint();
    const std::uint64_t total = r.varint();
    const std::uint64_t mn = r.varint();
    const std::uint64_t mx = r.varint();
    ms.stat.sum = static_cast<double>(total);
    ms.stat.min = static_cast<double>(mn);
    ms.stat.max = static_cast<double>(mx);
    ms.stat.domain = dftracer::utils::dataframe::FieldStatDomain::U64;
    ms.stat.esum = std::bit_cast<std::int64_t>(total);
    ms.stat.emin = std::bit_cast<std::int64_t>(mn);
    ms.stat.emax = std::bit_cast<std::int64_t>(mx);
    r.f64();  // mean is derived from sum/n; read to advance past the wire field
    ms.stat.sumsq = r.f64();
    ms.stat.m3 = r.f64();
    ms.stat.m4 = r.f64();
    if (fmt == METRIC_FMT_FULL_WITH_SKETCH) {
        auto blob = r.blob();
        ms.sketch = std::make_unique<DDSketch>(DDSketch::deserialize(
            reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size()));
    }
    return ms;
}

}  // namespace

void serialize_agg_key_into(std::string& out, AggMapType map_type,
                            const AggregationKey& key,
                            const StringIntern& intern) {
    out.clear();
    auto cat = intern.resolve(key.cat_id);
    auto name = intern.resolve(key.name_id);
    put_be16(out, compute_shard(cat, name, key.pid, key.tid));
    put_u8(out, static_cast<std::uint8_t>(map_type) |
                    (key.fhash_inline ? AGG_KEY_FHASH_INLINE : 0));
    put_varint(out, key.cat_id);
    put_varint(out, key.name_id);
    put_varint(out, key.pid);
    put_varint(out, key.tid);
    put_varint(out, key.hhash_id);
    if (key.fhash_inline) {
        put_be64(out, key.fhash);
    } else {
        put_varint(out, key.fhash);
    }
    put_varint(out, key.time_bucket);
    std::uint16_t num_extra =
        key.extra_keys ? static_cast<std::uint16_t>(key.extra_keys->size()) : 0;
    put_be16(out, num_extra);
    if (key.extra_keys) {
        for (const auto& [k, v] : *key.extra_keys) {
            put_varint(out, k);
            put_varint(out, v);
        }
    }
}

void serialize_agg_key_into(
    std::string& out, AggMapType map_type, std::string_view cat,
    std::string_view name, std::uint64_t pid, std::uint64_t tid,
    std::string_view hhash, std::string_view fhash, std::uint64_t time_bucket,
    StringIntern& intern,
    const std::vector<std::pair<std::string_view, std::string_view>>*
        extra_keys) {
    const std::uint16_t shard = compute_shard(cat, name, pid, tid);
    const std::uint16_t num_extra =
        extra_keys ? static_cast<std::uint16_t>(extra_keys->size()) : 0;

    std::size_t total = 2 + 1 + 6 * 5 + 8 + 2 + num_extra * 2 * 5;

    out.clear();
    out.reserve(total);

    put_be16(out, shard);
    const auto fhash_val = ::dftracer::utils::hash::parse_hex64(fhash);
    out.push_back(static_cast<char>(
        static_cast<std::uint8_t>(map_type) |
        (fhash_val ? AGG_KEY_FHASH_INLINE : std::uint8_t{0})));
    put_varint(out, intern.get_or_insert(cat));
    put_varint(out, intern.get_or_insert(name));
    put_varint(out, pid);
    put_varint(out, tid);
    put_varint(out, hhash.empty() ? 0 : intern.get_or_insert(hhash));
    if (fhash_val) {
        put_be64(out, *fhash_val);
    } else {
        put_varint(out, fhash.empty() ? 0 : intern.get_or_insert(fhash));
    }
    put_varint(out, time_bucket);
    put_be16(out, num_extra);
    if (extra_keys) {
        for (const auto& [k, v] : *extra_keys) {
            put_varint(out, intern.get_or_insert(k));
            put_varint(out, intern.get_or_insert(v));
        }
    }
}

std::string serialize_agg_key(AggMapType map_type, const AggregationKey& key,
                              const StringIntern& intern) {
    std::string out;
    out.reserve(47);
    serialize_agg_key_into(out, map_type, key, intern);
    return out;
}

DeserializedAggKey deserialize_agg_key(std::string_view data) {
    BinaryReader r(data);
    r.skip(2);  // shard
    const auto type_byte = r.u8();
    auto map_type = static_cast<AggMapType>(type_byte & ~AGG_KEY_FHASH_INLINE);
    AggregationKey key;
    key.fhash_inline = (type_byte & AGG_KEY_FHASH_INLINE) != 0;
    key.cat_id = static_cast<std::uint32_t>(r.varint());
    key.name_id = static_cast<std::uint32_t>(r.varint());
    key.pid = r.varint();
    key.tid = r.varint();
    key.hhash_id = static_cast<std::uint32_t>(r.varint());
    key.fhash = key.fhash_inline ? r.be64() : r.varint();
    key.time_bucket = r.varint();
    auto num_extra = r.be16();
    if (num_extra > 0) {
        key.extra_keys = std::make_unique<
            std::vector<std::pair<std::uint32_t, std::uint32_t>>>();
        key.extra_keys->reserve(num_extra);
        for (std::uint16_t i = 0; i < num_extra; ++i) {
            auto k = static_cast<std::uint32_t>(r.varint());
            auto v = static_cast<std::uint32_t>(r.varint());
            key.extra_keys->emplace_back(k, v);
        }
    }
    return {map_type, std::move(key)};
}

// Trails the value, so a reader that stops after the custom metrics ignores it.
constexpr std::uint64_t SKETCH_NONE = 0;
constexpr std::uint64_t SKETCH_DENSE = 1;
constexpr std::uint64_t SKETCH_SPARSE = 2;

static void put_distinct_sketch(
    std::string& out,
    const utilities::common::statistics::DistinctSketch& sketch) {
    const auto& dense = sketch.dense_registers();
    const auto& sparse = sketch.sparse_hashes();
    if (dense.empty() && sparse.empty()) {
        put_varint(out, SKETCH_NONE);
        return;
    }
    // The estimate rides along so a reader that only wants the count never
    // decodes the registers.
    if (!dense.empty()) {
        put_varint(out, SKETCH_DENSE);
        put_varint(out, sketch.estimate());
        out.append(reinterpret_cast<const char*>(dense.data()), dense.size());
        return;
    }
    auto unique = sparse;
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    put_varint(out, SKETCH_SPARSE);
    put_varint(out, static_cast<std::uint64_t>(unique.size()));
    put_varint(out, static_cast<std::uint64_t>(unique.size()));
    for (std::uint64_t h : unique) put_varint(out, h);
}

void serialize_agg_value_into(std::string& out, const AggregationMetrics& m) {
    // Fast path: no sketches anywhere. Pre-size to a conservative upper
    // bound and write directly via pointer, then shrink.
    bool has_sketch = m.duration.sketch || m.size.sketch || m.offset.sketch;
    if (!has_sketch && m.custom_metrics) {
        for (const auto& [_, ms] : *m.custom_metrics) {
            if (ms.sketch) {
                has_sketch = true;
                break;
            }
        }
    }

    if (!has_sketch && m.distinct_files.empty()) {
        std::size_t custom_bytes = 0;
        if (m.custom_metrics) {
            for (const auto& [name, _] : *m.custom_metrics) {
                custom_bytes +=
                    2 + name.size() + METRIC_STATS_MAX_BYTES_NO_SKETCH;
            }
        }
        const std::size_t max_total =
            10 /*count*/ + METRIC_STATS_MAX_BYTES_NO_SKETCH /*dur*/ +
            METRIC_STATS_MAX_BYTES_NO_SKETCH /*size*/ +
            METRIC_STATS_MAX_BYTES_NO_SKETCH /*offset*/ + 10 + 10 +
            10 /*ts/te/parent*/ + 10 /*num_custom*/ + custom_bytes +
            10 /*sketch length*/;
        out.resize(max_total);
        char* begin = out.data();
        char* p = begin;
        p = write_varint(p, m.count);
        p = write_metric_stats(p, m.duration);
        p = write_metric_stats(p, m.size);
        p = write_metric_stats(p, m.offset);
        p = write_varint(p, m.ts);
        p = write_varint(p, m.te);
        p = write_varint(p, m.parent_pid);
        const std::uint32_t num_custom =
            m.custom_metrics
                ? static_cast<std::uint32_t>(m.custom_metrics->size())
                : 0;
        p = write_varint(p, num_custom);
        if (m.custom_metrics) {
            for (const auto& [name, ms] : *m.custom_metrics) {
                p = write_str(p, name);
                p = write_metric_stats(p, ms);
            }
        }
        p = write_varint(p, SKETCH_NONE);  // no distinct-file sketch
        out.resize(static_cast<std::size_t>(p - begin));
        return;
    }

    out.clear();
    put_varint(out, m.count);
    serialize_metric_stats(out, m.duration);
    serialize_metric_stats(out, m.size);
    serialize_metric_stats(out, m.offset);
    put_varint(out, m.ts);
    put_varint(out, m.te);
    put_varint(out, m.parent_pid);

    std::uint32_t num_custom =
        m.custom_metrics ? static_cast<std::uint32_t>(m.custom_metrics->size())
                         : 0;
    put_varint(out, num_custom);
    if (m.custom_metrics) {
        for (const auto& [name, ms] : *m.custom_metrics) {
            put_str(out, name);
            serialize_metric_stats(out, ms);
        }
    }
    put_distinct_sketch(out, m.distinct_files);
}

std::string serialize_agg_value(const AggregationMetrics& m) {
    std::string out;
    out.reserve(256);
    serialize_agg_value_into(out, m);
    return out;
}

AggregationMetrics deserialize_agg_value(std::string_view data) {
    BinaryReader r(data);
    AggregationMetrics m;
    m.count = r.varint();
    m.duration = deserialize_metric_stats(r, m.sketch_accuracy);
    m.size = deserialize_metric_stats(r, m.sketch_accuracy);
    m.offset = deserialize_metric_stats(r, m.sketch_accuracy);
    m.ts = r.varint();
    m.te = r.varint();
    m.parent_pid = r.varint();

    auto num_custom = r.varint();
    if (num_custom > 0) {
        m.custom_metrics = std::make_unique<CustomMetricsMap>();
        for (std::uint32_t i = 0; i < num_custom; ++i) {
            auto name = r.str();
            auto ms = deserialize_metric_stats(r, m.sketch_accuracy);
            m.custom_metrics->emplace(std::string(name), std::move(ms));
        }
    }
    // Absent in values written before the sketch existed.
    if (r.has_remaining()) {
        auto tag = r.varint();
        if (tag != SKETCH_NONE) r.varint();  // estimate, recomputed below
        if (tag == SKETCH_DENSE) {
            constexpr auto n =
                utilities::common::statistics::DistinctSketch::REGISTERS;
            auto bytes = r.remaining();
            if (bytes.size() >= n) {
                const auto* p =
                    reinterpret_cast<const std::uint8_t*>(bytes.data());
                m.distinct_files.set_dense_registers(
                    std::vector<std::uint8_t>(p, p + n));
                r.skip(n);
            }
        } else if (tag == SKETCH_SPARSE) {
            auto n = r.varint();
            std::vector<std::uint64_t> hashes;
            hashes.reserve(static_cast<std::size_t>(n));
            for (std::uint64_t i = 0; i < n; ++i) hashes.push_back(r.varint());
            m.distinct_files.set_sparse_hashes(std::move(hashes));
        }
    }
    return m;
}

}  // namespace dftracer::utils::index::schemas::dft::agg
