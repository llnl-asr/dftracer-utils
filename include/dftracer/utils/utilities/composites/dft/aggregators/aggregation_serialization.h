#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_SERIALIZATION_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_SERIALIZATION_H

#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_output.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace dftracer::utils::utilities::composites::dft::aggregators {

static constexpr std::uint16_t AGG_KEY_NUM_SHARDS = 4096;

static constexpr std::uint8_t METRIC_FMT_COMPACT = 0;
static constexpr std::uint8_t METRIC_FMT_FULL = 1;
static constexpr std::uint8_t METRIC_FMT_FULL_WITH_SKETCH = 2;

// Intern dictionary: 0xFFFD + varint(id) -> string value
static constexpr char AGG_INTERN_DICT_PREFIX[] = "\xFF\xFD";
static constexpr std::size_t AGG_INTERN_DICT_PREFIX_LEN = 2;

// Global config: 0xFFFE -> time_interval_us (8) + config_hash (4)
static constexpr char AGG_GLOBAL_CONFIG_KEY[] = "\xFF\xFE";
static constexpr std::size_t AGG_GLOBAL_CONFIG_LEN = 12;

struct AggGlobalConfig {
    std::uint64_t time_interval_us = 0;
    std::uint32_t config_hash = 0;
};

inline std::string serialize_agg_global_config(const AggGlobalConfig& cfg) {
    std::string val(AGG_GLOBAL_CONFIG_LEN, '\0');
    val[0] = static_cast<char>((cfg.time_interval_us >> 56) & 0xFF);
    val[1] = static_cast<char>((cfg.time_interval_us >> 48) & 0xFF);
    val[2] = static_cast<char>((cfg.time_interval_us >> 40) & 0xFF);
    val[3] = static_cast<char>((cfg.time_interval_us >> 32) & 0xFF);
    val[4] = static_cast<char>((cfg.time_interval_us >> 24) & 0xFF);
    val[5] = static_cast<char>((cfg.time_interval_us >> 16) & 0xFF);
    val[6] = static_cast<char>((cfg.time_interval_us >> 8) & 0xFF);
    val[7] = static_cast<char>(cfg.time_interval_us & 0xFF);
    val[8] = static_cast<char>((cfg.config_hash >> 24) & 0xFF);
    val[9] = static_cast<char>((cfg.config_hash >> 16) & 0xFF);
    val[10] = static_cast<char>((cfg.config_hash >> 8) & 0xFF);
    val[11] = static_cast<char>(cfg.config_hash & 0xFF);
    return val;
}

inline AggGlobalConfig deserialize_agg_global_config(std::string_view data) {
    AggGlobalConfig cfg;
    if (data.size() >= AGG_GLOBAL_CONFIG_LEN) {
        cfg.time_interval_us =
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[0]))
             << 56) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[1]))
             << 48) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[2]))
             << 40) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[3]))
             << 32) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[4]))
             << 24) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[5]))
             << 16) |
            (static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[6]))
             << 8) |
            static_cast<std::uint64_t>(static_cast<std::uint8_t>(data[7]));
        cfg.config_hash =
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[8]))
             << 24) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[9]))
             << 16) |
            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[10]))
             << 8) |
            static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[11]));
    }
    return cfg;
}

// Per-file: 0xFFFF + file_id (4) -> empty value (presence = aggregated)
static constexpr char AGG_FILE_KEY_PREFIX[] = "\xFF\xFF";
static constexpr std::size_t AGG_FILE_KEY_PREFIX_LEN = 2;
static constexpr std::size_t AGG_FILE_KEY_LEN =
    AGG_FILE_KEY_PREFIX_LEN + sizeof(std::int32_t);

inline std::string make_agg_file_key(std::int32_t file_id) {
    std::string key(AGG_FILE_KEY_LEN, '\0');
    key[0] = AGG_FILE_KEY_PREFIX[0];
    key[1] = AGG_FILE_KEY_PREFIX[1];
    key[2] = static_cast<char>((file_id >> 24) & 0xFF);
    key[3] = static_cast<char>((file_id >> 16) & 0xFF);
    key[4] = static_cast<char>((file_id >> 8) & 0xFF);
    key[5] = static_cast<char>(file_id & 0xFF);
    return key;
}

void serialize_agg_key_into(std::string& out, std::uint32_t config_hash,
                            AggMapType map_type, const AggregationKey& key);

void serialize_agg_key_into(
    std::string& out, std::uint32_t config_hash, AggMapType map_type,
    std::string_view cat, std::string_view name, std::uint64_t pid,
    std::uint64_t tid, std::string_view hhash, std::string_view fhash,
    std::uint64_t time_bucket,
    const std::vector<std::pair<std::string_view, std::string_view>>*
        extra_keys = nullptr);
std::string serialize_agg_key(std::uint32_t config_hash, AggMapType map_type,
                              const AggregationKey& key);

struct DeserializedAggKey {
    std::uint32_t config_hash;
    AggMapType map_type;
    AggregationKey key;
};
DeserializedAggKey deserialize_agg_key(std::string_view data);

/// Key view with resolved strings from the intern table.
/// Lifetime: valid as long as aggregation_intern() exists (process lifetime).
struct AggKeyView {
    AggMapType map_type;
    std::string_view cat;
    std::string_view name;
    std::uint64_t pid;
    std::uint64_t tid;
    std::string_view hhash;
    std::string_view fhash;
    std::uint64_t time_bucket;
};

/// Parse aggregation key: reads varint intern IDs and resolves to strings.
/// Returns false if parsing fails.
inline bool parse_agg_key_view(std::string_view data, AggKeyView& out) {
    if (data.size() < 6) return false;

    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    const auto* end = p + data.size();

    p += 2;  // shard

    out.map_type = static_cast<AggMapType>(*p++);

    auto read_varint = [&]() -> std::uint64_t {
        std::uint64_t v = 0;
        unsigned shift = 0;
        while (p < end) {
            auto b = *p++;
            v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return v;
            shift += 7;
        }
        return v;
    };

    auto& intern = aggregation_intern();
    auto cat_id = static_cast<std::uint32_t>(read_varint());
    auto name_id = static_cast<std::uint32_t>(read_varint());
    out.pid = read_varint();
    out.tid = read_varint();
    auto hhash_id = static_cast<std::uint32_t>(read_varint());
    auto fhash_id = static_cast<std::uint32_t>(read_varint());
    out.time_bucket = read_varint();

    out.cat = intern.resolve(cat_id);
    out.name = intern.resolve(name_id);
    out.hhash = hhash_id ? intern.resolve(hhash_id) : std::string_view{};
    out.fhash = fhash_id ? intern.resolve(fhash_id) : std::string_view{};

    return true;
}

void serialize_agg_value_into(std::string& out,
                              const AggregationMetrics& metrics);
std::string serialize_agg_value(const AggregationMetrics& metrics);
AggregationMetrics deserialize_agg_value(std::string_view data);

/// Lightweight metrics view for Arrow export - only the fields needed.
struct AggMetricsView {
    std::uint64_t count;
    std::uint64_t dur_total;
    std::uint64_t dur_min;
    std::uint64_t dur_max;
    std::uint64_t size_total;
    std::uint64_t size_min;
    std::uint64_t size_max;
    std::uint64_t offset_total;
    std::uint64_t offset_min;
    std::uint64_t offset_max;
    std::uint64_t ts;
    std::uint64_t te;
};

/// Full metrics view including mean/m2 for stddev computation.
/// Use for iter_aggregation which needs mean and stddev columns.
struct AggMetricsFullView {
    std::uint64_t count;
    std::uint64_t dur_total;
    std::uint64_t dur_min;
    std::uint64_t dur_max;
    double dur_mean;
    double dur_m2;  // For Welford's stddev: stddev = sqrt(m2 / count)
    std::uint64_t size_total;
    std::uint64_t size_min;
    std::uint64_t size_max;
    double size_mean;
    double size_m2;
    std::uint64_t offset_total;
    std::uint64_t offset_min;
    std::uint64_t offset_max;
    double offset_mean;
    double offset_m2;
    std::uint64_t ts;
    std::uint64_t te;

    double dur_stddev() const {
        return count > 1 ? std::sqrt(dur_m2 / static_cast<double>(count)) : 0.0;
    }
    double size_stddev() const {
        return count > 1 ? std::sqrt(size_m2 / static_cast<double>(count))
                         : 0.0;
    }
    double offset_stddev() const {
        return count > 1 ? std::sqrt(offset_m2 / static_cast<double>(count))
                         : 0.0;
    }
};

/// Fast value parser for Arrow export - skips mean/m2/m3/m4/sketch.
inline bool parse_agg_value_view(std::string_view data, AggMetricsView& out) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    const auto* end = p + data.size();

    auto read_varint = [&]() -> std::uint64_t {
        std::uint64_t v = 0;
        int shift = 0;
        while (p < end) {
            std::uint8_t b = *p++;
            v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
        }
        return v;
    };

    auto skip_f64 = [&]() { p += 8; };

    auto read_metric_stats_partial =
        [&](std::uint64_t& total, std::uint64_t& min, std::uint64_t& max) {
            auto fmt = read_varint();
            if (fmt == METRIC_FMT_COMPACT) {
                auto val = read_varint();
                total = min = max = val;
                return;
            }
            read_varint();  // skip count
            total = read_varint();
            min = read_varint();
            max = read_varint();
            skip_f64();  // mean
            skip_f64();  // m2
            if (fmt == METRIC_FMT_FULL_WITH_SKETCH) {
                auto len = read_varint();
                p += len;
            }
        };

    if (p >= end) return false;

    out.count = read_varint();
    read_metric_stats_partial(out.dur_total, out.dur_min, out.dur_max);
    read_metric_stats_partial(out.size_total, out.size_min, out.size_max);
    read_metric_stats_partial(out.offset_total, out.offset_min, out.offset_max);
    out.ts = read_varint();
    out.te = read_varint();

    return true;
}

/// Full value parser for iter_aggregation - includes mean/m2 for stddev.
inline bool parse_agg_value_full_view(std::string_view data,
                                      AggMetricsFullView& out) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    const auto* end = p + data.size();

    auto read_varint = [&]() -> std::uint64_t {
        std::uint64_t v = 0;
        int shift = 0;
        while (p < end) {
            std::uint8_t b = *p++;
            v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
        }
        return v;
    };

    auto read_f64 = [&]() -> double {
        if (p + 8 > end) return 0.0;
        std::uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) {
            bits |= static_cast<std::uint64_t>(*p++) << (i * 8);
        }
        double result;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    };

    auto read_metric_stats_full = [&](std::uint64_t& total, std::uint64_t& min,
                                      std::uint64_t& max, double& mean,
                                      double& m2) {
        auto fmt = read_varint();
        if (fmt == METRIC_FMT_COMPACT) {
            auto val = read_varint();
            total = min = max = val;
            mean = static_cast<double>(val);
            m2 = 0.0;
            return;
        }
        read_varint();  // skip count (use outer count)
        total = read_varint();
        min = read_varint();
        max = read_varint();
        mean = read_f64();
        m2 = read_f64();
        if (fmt == METRIC_FMT_FULL_WITH_SKETCH) {
            auto len = read_varint();
            p += len;
        }
    };

    if (p >= end) return false;

    out.count = read_varint();
    read_metric_stats_full(out.dur_total, out.dur_min, out.dur_max,
                           out.dur_mean, out.dur_m2);
    read_metric_stats_full(out.size_total, out.size_min, out.size_max,
                           out.size_mean, out.size_m2);
    read_metric_stats_full(out.offset_total, out.offset_min, out.offset_max,
                           out.offset_mean, out.offset_m2);
    out.ts = read_varint();
    out.te = read_varint();

    return true;
}

/// Load intern dictionary from RocksDB into aggregation_intern().
void load_intern_dictionary(dftracer::utils::rocksdb::RocksDatabase& db);

/// Flush any new intern entries to RocksDB as 0xFFFD keys.
void flush_intern_dictionary(
    dftracer::utils::rocksdb::RocksDatabase& db,
    dftracer::utils::rocksdb::RocksDatabase::Batch& batch);

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

namespace dftracer::utils::utilities::indexer {
class IndexBatchSink;
}

namespace dftracer::utils::utilities::composites::dft::aggregators {

/// Sink-backed overload: flushes new intern entries via
/// `IndexBatchSink::insert_aggregation_put`. Used by the distributed SST
/// pipeline where the visitor writes to an SST instead of a live DB.
void flush_intern_dictionary(
    dftracer::utils::utilities::indexer::IndexBatchSink& sink);

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_SERIALIZATION_H
