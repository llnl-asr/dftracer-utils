#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_SERIALIZATION_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_SERIALIZATION_H

#include <dftracer/utils/core/common/hash/hex64.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_intern.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_output.h>
#include <dftracer/utils/index/store/database.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::index::schemas::dft::agg {

static constexpr std::uint16_t AGG_KEY_NUM_SHARDS = 4096;

static constexpr std::uint8_t METRIC_FMT_COMPACT = 0;
static constexpr std::uint8_t METRIC_FMT_FULL = 1;
static constexpr std::uint8_t METRIC_FMT_FULL_WITH_SKETCH = 2;

void serialize_agg_key_into(std::string& out, AggMapType map_type,
                            const AggregationKey& key,
                            const StringIntern& intern);

void serialize_agg_key_into(
    std::string& out, AggMapType map_type, std::string_view cat,
    std::string_view name, std::uint64_t pid, std::uint64_t tid,
    std::string_view hhash, std::string_view fhash, std::uint64_t time_bucket,
    StringIntern& intern,
    const std::vector<std::pair<std::string_view, std::string_view>>*
        extra_keys = nullptr);
std::string serialize_agg_key(AggMapType map_type, const AggregationKey& key,
                              const StringIntern& intern);

struct DeserializedAggKey {
    AggMapType map_type;
    AggregationKey key;
};
DeserializedAggKey deserialize_agg_key(std::string_view data);

/// Key view with resolved strings from the intern table.
/// Lifetime: valid as long as the table that resolved them.
struct AggKeyView {
    AggMapType map_type;
    std::string_view cat;
    std::string_view name;
    std::uint64_t pid;
    std::uint64_t tid;
    std::string_view hhash;
    /// The file hash itself, or an intern id when `fhash_inline` is false, in
    /// which case `fhash_str` carries the original text.
    std::uint64_t fhash;
    bool fhash_inline;
    std::string_view fhash_str;
    std::uint64_t time_bucket;
    /// Resolved extra key/value pairs carried in the key (e.g. epoch/step on
    /// PROFILE rows). Empty for EVENT/SYSTEM keys. Populated by
    /// parse_agg_key_view only when `want_extra_keys` is requested.
    std::vector<std::pair<std::string_view, std::string_view>> extra_keys;
};

/// Hex text of `kv`'s file hash, rendered into `buf` for inline hashes.
inline std::string_view fhash_text(
    const AggKeyView& kv, char (&buf)[::dftracer::utils::hash::HEX64_DIGITS]) {
    if (!kv.fhash_inline) return kv.fhash_str;
    if (kv.fhash == 0) return {};
    ::dftracer::utils::hash::format_hex64(kv.fhash, buf);
    return std::string_view(buf, sizeof(buf));
}

/// Bit in the map-type byte marking an inline 8-byte file hash, so the
/// interned fallback costs no extra key bytes.
inline constexpr std::uint8_t AGG_KEY_FHASH_INLINE = 0x80;

/// Decode a LEB128 varint, advancing `p` (bounded by `end`).
inline std::uint64_t decode_varint(const std::uint8_t*& p,
                                   const std::uint8_t* end) {
    std::uint64_t v = 0;
    unsigned shift = 0;
    while (p < end) {
        std::uint8_t b = *p++;
        v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) break;
        shift += 7;
    }
    return v;
}

/// Parse aggregation key: reads varint intern IDs and resolves to strings.
/// Returns false if parsing fails. When `want_extra_keys` is set, also decodes
/// the trailing key/value pairs into `out.extra_keys` (PROFILE epoch/step); the
/// hot EVENT path leaves it false so no extra work is done.
inline bool parse_agg_key_view(std::string_view data,
                               const StringIntern& intern, AggKeyView& out,
                               bool want_extra_keys = false) {
    if (data.size() < 6) return false;

    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    const auto* end = p + data.size();

    p += 2;  // shard

    const std::uint8_t type_byte = *p++;
    out.map_type = static_cast<AggMapType>(type_byte & ~AGG_KEY_FHASH_INLINE);
    out.fhash_inline = (type_byte & AGG_KEY_FHASH_INLINE) != 0;

    auto read_varint = [&]() { return decode_varint(p, end); };

    auto cat_id = static_cast<std::uint32_t>(read_varint());
    auto name_id = static_cast<std::uint32_t>(read_varint());
    out.pid = read_varint();
    out.tid = read_varint();
    auto hhash_id = static_cast<std::uint32_t>(read_varint());
    if (out.fhash_inline) {
        if (end - p < 8) return false;
        out.fhash = 0;
        for (int i = 0; i < 8; ++i)
            out.fhash = (out.fhash << 8) | static_cast<std::uint64_t>(*p++);
    } else {
        out.fhash = read_varint();
        out.fhash_str =
            out.fhash ? intern.resolve(static_cast<std::uint32_t>(out.fhash))
                      : std::string_view{};
    }
    out.time_bucket = read_varint();

    out.extra_keys.clear();
    if (want_extra_keys && end - p >= 2) {
        // num_extra is a big-endian u16 (put_be16), not a varint.
        const std::uint16_t num_extra = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
        p += 2;
        out.extra_keys.reserve(num_extra);
        for (std::uint16_t i = 0; i < num_extra && p < end; ++i) {
            const auto k = static_cast<std::uint32_t>(read_varint());
            const auto v = static_cast<std::uint32_t>(read_varint());
            out.extra_keys.emplace_back(intern.resolve(k), intern.resolve(v));
        }
    }

    out.cat = intern.resolve(cat_id);
    out.name = intern.resolve(name_id);
    out.hhash = hhash_id ? intern.resolve(hhash_id) : std::string_view{};

    return true;
}

void serialize_agg_value_into(std::string& out,
                              const AggregationMetrics& metrics);
std::string serialize_agg_value(const AggregationMetrics& metrics);
AggregationMetrics deserialize_agg_value(std::string_view data);

/// Lightweight metrics view for Arrow export - only the fields needed.
struct AggMetricsView {
    std::uint64_t count;
    /// 0 when the file hash is in the key, or the index predates the sketch.
    std::uint64_t distinct_files = 0;
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
    /// The running mean is dur_mean + dur_mean_lo (a value near it, and the
    /// small remainder); dur_m2/m3/m4 are the central moments about it.
    double dur_mean;
    double dur_mean_lo;
    double dur_m2;
    double dur_m3;
    double dur_m4;
    std::uint64_t size_total;
    std::uint64_t size_min;
    std::uint64_t size_max;
    double size_mean;
    double size_mean_lo;
    double size_m2;
    double size_m3;
    double size_m4;
    std::uint64_t offset_total;
    std::uint64_t offset_min;
    std::uint64_t offset_max;
    double offset_mean;
    double offset_mean_lo;
    double offset_m2;
    double offset_m3;
    double offset_m4;
    std::uint64_t ts;
    std::uint64_t te;
    /// Serialized DDSketch blobs (empty when the tier stored no sketch),
    /// viewing into the parsed value buffer - valid only while that buffer is
    /// alive.
    std::string_view dur_sketch;
    std::string_view size_sketch;

    /// m2 is the central sum of squares, sum (x - mean)^2. Population stddev
    /// = sqrt(m2 / n).
    static double stddev_from(std::uint64_t n, double m2) {
        if (n <= 1) return 0.0;
        return m2 > 0.0 ? std::sqrt(m2 / static_cast<double>(n)) : 0.0;
    }
};

/// Fast value parser for Arrow export - skips mean/m2/m3/m4/sketch.
inline bool parse_agg_value_view(std::string_view data, AggMetricsView& out) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    const auto* end = p + data.size();

    auto read_varint = [&]() { return decode_varint(p, end); };

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
            skip_f64();  // mean_lo
            skip_f64();  // m2
            skip_f64();  // m3
            skip_f64();  // m4
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

    out.distinct_files = 0;
    if (p < end) {
        read_varint();  // parent_pid
        auto num_custom = read_varint();
        for (std::uint64_t i = 0; i < num_custom && p + 2 <= end; ++i) {
            // put_str writes a big-endian 16-bit length, not a varint.
            std::size_t name_len = (static_cast<std::size_t>(p[0]) << 8) | p[1];
            p += 2 + name_len;
            std::uint64_t c_total, c_min, c_max;
            read_metric_stats_partial(c_total, c_min, c_max);
        }
        if (p < end) {
            auto tag = read_varint();
            if (tag != 0) out.distinct_files = read_varint();
        }
    }

    return true;
}

/// Full value parser for iter_aggregation - includes mean/m2 for stddev.
inline bool parse_agg_value_full_view(std::string_view data,
                                      AggMetricsFullView& out) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    const auto* end = p + data.size();

    auto read_varint = [&]() { return decode_varint(p, end); };

    auto read_f64 = [&]() -> double {
        if (p + 8 > end) return 0.0;
        // Big-endian, matching put_double/put_be64 on the write side.
        std::uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) {
            bits = (bits << 8) | static_cast<std::uint64_t>(*p++);
        }
        double result;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    };

    auto read_metric_stats_full = [&](std::uint64_t& total, std::uint64_t& min,
                                      std::uint64_t& max, double& mean,
                                      double& mean_lo, double& m2, double& m3,
                                      double& m4, std::string_view* sketch) {
        auto fmt = read_varint();
        if (fmt == METRIC_FMT_COMPACT) {
            auto val = read_varint();
            total = min = max = val;
            const double v = static_cast<double>(val);
            mean = v;
            mean_lo = 0.0;
            m2 = m3 = m4 = 0.0;  // one value: no spread about its mean
            return;
        }
        read_varint();           // skip count (use outer count)
        total = read_varint();
        min = read_varint();
        max = read_varint();
        mean = read_f64();
        mean_lo = read_f64();
        m2 = read_f64();
        m3 = read_f64();
        m4 = read_f64();
        if (fmt == METRIC_FMT_FULL_WITH_SKETCH) {
            auto len = read_varint();
            if (sketch)
                *sketch =
                    std::string_view(reinterpret_cast<const char*>(p), len);
            p += len;
        }
    };

    if (p >= end) return false;

    out.count = read_varint();
    read_metric_stats_full(out.dur_total, out.dur_min, out.dur_max,
                           out.dur_mean, out.dur_mean_lo, out.dur_m2,
                           out.dur_m3, out.dur_m4, &out.dur_sketch);
    read_metric_stats_full(out.size_total, out.size_min, out.size_max,
                           out.size_mean, out.size_mean_lo, out.size_m2,
                           out.size_m3, out.size_m4, &out.size_sketch);
    read_metric_stats_full(out.offset_total, out.offset_min, out.offset_max,
                           out.offset_mean, out.offset_mean_lo, out.offset_m2,
                           out.offset_m3, out.offset_m4, nullptr);
    out.ts = read_varint();
    out.te = read_varint();

    return true;
}

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif  // DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGGREGATION_SERIALIZATION_H
