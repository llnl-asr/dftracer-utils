#include <dftracer/utils/utilities/common/serialization/binary_codec.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>
#include <dftracer/utils/utilities/hash/fnv1a_hasher_utility.h>
#include <dftracer/utils/utilities/indexer/index_batch_sink.h>

#include <cstring>

namespace dftracer::utils::utilities::composites::dft::aggregators {

namespace {

namespace hash = dftracer::utils::utilities::hash;

using common::serialization::BinaryReader;
using common::serialization::put_be16;
using common::serialization::put_blob;
using common::serialization::put_double;
using common::serialization::put_str;
using common::serialization::put_u8;
using common::serialization::put_varint;
using common::serialization::write_double;
using common::serialization::write_str;
using common::serialization::write_varint;

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

    hash::Fnv1aHashBuilder h;
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
// so consumers that don't need stddev can skip the power sums.
inline char* write_metric_stats(char* p, const MetricStats& ms) {
    // COMPACT format can only represent "empty" (count=0) or a single
    // event with value = total (count=1). Critically: count=1 total=0 is
    // a VALID state (one event with value 0) that COMPACT cannot round-
    // trip because the deserializer falls back to count=0 whenever the
    // serialized varint is 0. Avoid COMPACT for that case.
    const bool compact_empty =
        ms.count == 0 && ms.total == 0 && ms.m2 == 0.0 && !ms.sketch;
    const bool compact_single = ms.count == 1 && ms.total > 0 &&
                                ms.m2 == static_cast<double>(ms.total) *
                                             static_cast<double>(ms.total) &&
                                !ms.sketch;
    if (compact_empty || compact_single) {
        *p++ = static_cast<char>(METRIC_FMT_COMPACT);
        return write_varint(p, ms.count == 0 ? 0 : ms.total);
    }
    *p++ = static_cast<char>(METRIC_FMT_FULL);
    p = write_varint(p, ms.count);
    p = write_varint(p, ms.total);
    p = write_varint(p, ms.min);
    p = write_varint(p, ms.max);
    p = write_double(p, ms.mean);
    p = write_double(p, ms.m2);
    // m3/m4 not persisted yet -- skewness/kurtosis recomputed in memory.
    // p = write_double(p, ms.m3);
    // p = write_double(p, ms.m4);
    return p;
}

// Upper bound for MetricStats (FULL fmt, no sketch):
//   1 (fmt) + 4*10 (varints) + 2*8 (doubles) = 57 bytes
constexpr std::size_t METRIC_STATS_MAX_BYTES_NO_SKETCH = 57;

void serialize_metric_stats(std::string& out, const MetricStats& ms) {
    if (!ms.sketch) {
        const auto old_size = out.size();
        out.resize(old_size + METRIC_STATS_MAX_BYTES_NO_SKETCH);
        char* begin = out.data() + old_size;
        char* p = write_metric_stats(begin, ms);
        out.resize(old_size + static_cast<std::size_t>(p - begin));
        return;
    }
    put_u8(out, METRIC_FMT_FULL_WITH_SKETCH);
    put_varint(out, ms.count);
    put_varint(out, ms.total);
    put_varint(out, ms.min);
    put_varint(out, ms.max);
    put_double(out, ms.mean);
    put_double(out, ms.m2);
    // m3/m4 not persisted yet.
    // put_double(out, ms.m3);
    // put_double(out, ms.m4);
    auto blob = ms.sketch->serialize();
    put_blob(out, blob);
}

MetricStats deserialize_metric_stats(BinaryReader& r, double accuracy) {
    auto fmt = r.u8();
    if (fmt == METRIC_FMT_COMPACT) {
        MetricStats ms(accuracy);
        auto val = r.varint();
        if (val > 0) {
            ms.count = 1;
            ms.total = val;
            ms.min = val;
            ms.max = val;
            ms.mean = static_cast<double>(val);
            const double v = static_cast<double>(val);
            ms.m2 = v * v;
            // In-memory skewness/kurtosis only; not persisted:
            // ms.m3 = v * v * v;
            // ms.m4 = v * v * v * v;
        }
        return ms;
    }
    MetricStats ms(accuracy);
    ms.count = r.varint();
    ms.total = r.varint();
    ms.min = r.varint();
    ms.max = r.varint();
    ms.mean = r.f64();
    ms.m2 = r.f64();
    // ms.m3 = r.f64();
    // ms.m4 = r.f64();
    if (fmt == METRIC_FMT_FULL_WITH_SKETCH) {
        auto blob = r.blob();
        ms.sketch = std::make_unique<DDSketch>(DDSketch::deserialize(
            reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size()));
    }
    return ms;
}

}  // namespace

void serialize_agg_key_into(std::string& out, std::uint32_t /*config_hash*/,
                            AggMapType map_type, const AggregationKey& key) {
    out.clear();
    auto& intern = aggregation_intern();
    auto cat = intern.resolve(key.cat_id);
    auto name = intern.resolve(key.name_id);
    put_be16(out, compute_shard(cat, name, key.pid, key.tid));
    put_u8(out, static_cast<std::uint8_t>(map_type));
    put_varint(out, key.cat_id);
    put_varint(out, key.name_id);
    put_varint(out, key.pid);
    put_varint(out, key.tid);
    put_varint(out, key.hhash_id);
    put_varint(out, key.fhash_id);
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
    std::string& out, std::uint32_t /*config_hash*/, AggMapType map_type,
    std::string_view cat, std::string_view name, std::uint64_t pid,
    std::uint64_t tid, std::string_view hhash, std::string_view fhash,
    std::uint64_t time_bucket,
    const std::vector<std::pair<std::string_view, std::string_view>>*
        extra_keys) {
    auto& intern = aggregation_intern();
    const std::uint16_t shard = compute_shard(cat, name, pid, tid);
    const std::uint16_t num_extra =
        extra_keys ? static_cast<std::uint16_t>(extra_keys->size()) : 0;

    // All fields are varints now — conservative upper bound
    std::size_t total = 2 + 1 + 7 * 5 + 2 + num_extra * 2 * 5;

    out.clear();
    out.reserve(total);

    put_be16(out, shard);
    out.push_back(static_cast<char>(map_type));
    put_varint(out, intern.get_or_insert(cat));
    put_varint(out, intern.get_or_insert(name));
    put_varint(out, pid);
    put_varint(out, tid);
    put_varint(out, hhash.empty() ? 0 : intern.get_or_insert(hhash));
    put_varint(out, fhash.empty() ? 0 : intern.get_or_insert(fhash));
    put_varint(out, time_bucket);
    put_be16(out, num_extra);
    if (extra_keys) {
        for (const auto& [k, v] : *extra_keys) {
            put_varint(out, intern.get_or_insert(k));
            put_varint(out, intern.get_or_insert(v));
        }
    }
}

std::string serialize_agg_key(std::uint32_t config_hash, AggMapType map_type,
                              const AggregationKey& key) {
    std::string out;
    out.reserve(47);
    serialize_agg_key_into(out, config_hash, map_type, key);
    return out;
}

DeserializedAggKey deserialize_agg_key(std::string_view data) {
    BinaryReader r(data);
    (void)r.be16();
    auto map_type = static_cast<AggMapType>(r.u8());
    AggregationKey key;
    key.cat_id = static_cast<std::uint32_t>(r.varint());
    key.name_id = static_cast<std::uint32_t>(r.varint());
    key.pid = r.varint();
    key.tid = r.varint();
    key.hhash_id = static_cast<std::uint32_t>(r.varint());
    key.fhash_id = static_cast<std::uint32_t>(r.varint());
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
    return {0, map_type, std::move(key)};
}

void serialize_agg_value_into(std::string& out, const AggregationMetrics& m) {
    // Fast path: no sketches anywhere. Pre-size to a conservative upper
    // bound and write directly via pointer, then shrink.
    bool has_sketch = m.duration.sketch || m.size.sketch;
    if (!has_sketch && m.custom_metrics) {
        for (const auto& [_, ms] : *m.custom_metrics) {
            if (ms.sketch) {
                has_sketch = true;
                break;
            }
        }
    }

    if (!has_sketch) {
        std::size_t custom_bytes = 0;
        if (m.custom_metrics) {
            for (const auto& [name, _] : *m.custom_metrics) {
                custom_bytes +=
                    2 + name.size() + METRIC_STATS_MAX_BYTES_NO_SKETCH;
            }
        }
        const std::size_t max_total =
            10 /*count*/ + METRIC_STATS_MAX_BYTES_NO_SKETCH /*dur*/ +
            METRIC_STATS_MAX_BYTES_NO_SKETCH /*size*/ + 10 + 10 +
            10 /*ts/te/parent*/ + 10 /*num_custom*/ + custom_bytes;
        out.resize(max_total);
        char* begin = out.data();
        char* p = begin;
        p = write_varint(p, m.count);
        p = write_metric_stats(p, m.duration);
        p = write_metric_stats(p, m.size);
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
        out.resize(static_cast<std::size_t>(p - begin));
        return;
    }

    out.clear();
    put_varint(out, m.count);
    serialize_metric_stats(out, m.duration);
    serialize_metric_stats(out, m.size);
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
    return m;
}

namespace {
std::atomic<std::uint32_t>& intern_flushed_watermark() {
    static std::atomic<std::uint32_t> watermark{0};
    return watermark;
}
}  // namespace

void load_intern_dictionary(dftracer::utils::rocksdb::RocksDatabase& db) {
    namespace rcf = dftracer::utils::rocksdb::cf;
    auto& intern = aggregation_intern();
    auto it = db.new_iterator(rcf::AGGREGATION);
    std::uint32_t max_id_plus_one = 0;
    for (it->Seek({AGG_INTERN_DICT_PREFIX, AGG_INTERN_DICT_PREFIX_LEN});
         it->Valid(); it->Next()) {
        auto key_slice = it->key();
        if (key_slice.size() < AGG_INTERN_DICT_PREFIX_LEN) break;
        if (static_cast<std::uint8_t>(key_slice[0]) != 0xFF ||
            static_cast<std::uint8_t>(key_slice[1]) != 0xFD)
            break;

        // Decode the id encoded as varint after the prefix. RocksDB key order
        // is lex, which is NOT varint-numeric order past 127, so we cannot
        // infer the id from iteration order. Read it explicitly.
        common::serialization::BinaryReader key_reader(
            std::string_view(key_slice.data() + AGG_INTERN_DICT_PREFIX_LEN,
                             key_slice.size() - AGG_INTERN_DICT_PREFIX_LEN));
        std::uint32_t id = 0;
        try {
            id = static_cast<std::uint32_t>(key_reader.varint());
        } catch (const std::exception&) {
            continue;
        }

        auto val_slice = it->value();
        intern.insert_at_id(
            id, std::string_view(val_slice.data(), val_slice.size()));
        if (id + 1u > max_id_plus_one) max_id_plus_one = id + 1u;
    }
    intern_flushed_watermark().store(max_id_plus_one,
                                     std::memory_order_relaxed);
}

void flush_intern_dictionary(
    dftracer::utils::rocksdb::RocksDatabase& db,
    dftracer::utils::rocksdb::RocksDatabase::Batch& batch) {
    namespace rcf = dftracer::utils::rocksdb::cf;
    auto& intern = aggregation_intern();
    auto current = static_cast<std::uint32_t>(intern.size());
    auto flushed = intern_flushed_watermark().load(std::memory_order_relaxed);
    if (current <= flushed) return;

    for (std::uint32_t id = flushed; id < current; ++id) {
        std::string key(AGG_INTERN_DICT_PREFIX, AGG_INTERN_DICT_PREFIX_LEN);
        common::serialization::put_varint(key, id);
        auto sv = intern.resolve(id);
        db.put(batch, rcf::AGGREGATION, key,
               std::string_view(sv.data(), sv.size()));
    }

    // CAS to advance watermark; another thread may have already advanced it
    while (flushed < current) {
        if (intern_flushed_watermark().compare_exchange_weak(
                flushed, current, std::memory_order_relaxed))
            break;
        if (flushed >= current) break;
    }
}

void flush_intern_dictionary(
    dftracer::utils::utilities::indexer::IndexBatchSink& sink) {
    auto& intern = aggregation_intern();
    auto current = static_cast<std::uint32_t>(intern.size());
    auto flushed = intern_flushed_watermark().load(std::memory_order_relaxed);
    if (current <= flushed) return;

    std::string key;
    for (std::uint32_t id = flushed; id < current; ++id) {
        key.assign(AGG_INTERN_DICT_PREFIX, AGG_INTERN_DICT_PREFIX_LEN);
        common::serialization::put_varint(key, id);
        auto sv = intern.resolve(id);
        sink.insert_aggregation_put(key,
                                    std::string_view(sv.data(), sv.size()));
    }

    while (flushed < current) {
        if (intern_flushed_watermark().compare_exchange_weak(
                flushed, current, std::memory_order_relaxed))
            break;
        if (flushed >= current) break;
    }
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
