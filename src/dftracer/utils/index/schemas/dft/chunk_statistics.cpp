// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/event_cost.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/json/record_parser.h>
#include <simdjson.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace dftracer::utils::index::schemas::dft {

namespace {

using dataframe::DDSketch;
using dataframe::Log2Histogram;

template <class V>
std::uint32_t slot_of(dstd::StringViewMap<V>& map, dstd::StringView key) {
    auto it = map.find(key);
    if (it == map.end()) it = map.emplace(dstd::String(key), V{}).first;
    return static_cast<std::uint32_t>(it - map.begin());
}

template <class V>
V& at_slot(dstd::StringViewMap<V>& map, std::uint32_t slot) {
    return (map.begin() + slot)->second;
}

}  // namespace

void ChunkStatistics::update_from_event(dstd::StringView name,
                                        dstd::StringView cat, std::uint64_t pid,
                                        std::uint64_t tid, std::uint64_t ts,
                                        std::uint64_t dur, bool has_dur) {
    NameSlots names;
    PidSlots pids;
    update_from_event(names, pids, name, cat, pid, tid, ts, dur, has_dur);
}

void ChunkStatistics::update_from_event(NameSlots& names, PidSlots& pids,
                                        dstd::StringView name,
                                        dstd::StringView cat, std::uint64_t pid,
                                        std::uint64_t tid, std::uint64_t ts,
                                        std::uint64_t dur, bool has_dur) {
    ++total_events;

    if (names.category_count == NO_SLOT) {
#if DFTRACER_UTILS_TEST_PROBES
        index::event_cost::g_stats_string_lookups.fetch_add(
            1, std::memory_order_relaxed);
#endif
        names.category_count = slot_of(category_counts, cat);
        names.name_count = slot_of(name_counts, name);
        if (name_category.find(name) == name_category.end())
            name_category.emplace(dstd::String(name), cat);
    }
    if (pids.pid_tid_count == NO_SLOT) {
        constexpr std::size_t pid_tid_buf_size =
            (2 * std::numeric_limits<std::uint64_t>::digits10) + 3;
        char pt_buf[pid_tid_buf_size];
        auto [pp, ec1] = dstd::to_chars(pt_buf, pt_buf + sizeof(pt_buf), pid);
        if (ec1 != std::errc{} || pp == pt_buf + sizeof(pt_buf)) {
            throw common::DFTUtilsException(common::ErrorCode::Internal,
                                            "failed to format pid");
        }
        *pp++ = ':';
        auto [tp, ec2] = dstd::to_chars(pp, pt_buf + sizeof(pt_buf), tid);
        if (ec2 != std::errc{}) {
            throw common::DFTUtilsException(common::ErrorCode::Internal,
                                            "failed to format tid");
        }
        pids.pid_tid_count =
            slot_of(pid_tid_counts, dstd::StringView(pt_buf, tp - pt_buf));
    }
    ++at_slot(category_counts, names.category_count);
    ++at_slot(name_counts, names.name_count);
    ++at_slot(pid_tid_counts, pids.pid_tid_count);

    if (ts < min_timestamp_us) min_timestamp_us = ts;
    if (ts > 0 && ts < min_nonzero_timestamp_us) min_nonzero_timestamp_us = ts;
    std::uint64_t end_ts = ts + dur;
    if (end_ts > max_timestamp_us) max_timestamp_us = end_ts;
    timestamp_histogram.add(ts);

    // Duration aggregates (global and per-name) count only events that carry a
    // duration, so a counter or instant with no dur field never lands in them.
    if (!has_dur) return;

    // Welford's online algorithm for variance
    const double old_mean = duration_mean();

    ++duration_count;
    duration_sum_us = saturating_duration_sum(duration_sum_us, dur);
    if (dur < duration_min_us) duration_min_us = dur;
    if (dur > duration_max_us) duration_max_us = dur;

    const double delta = static_cast<double>(dur) - old_mean;
    // Once the sum has saturated it no longer gives the mean.
    const double new_mean =
        duration_sum_us == std::numeric_limits<std::int64_t>::max()
            ? old_mean + delta / static_cast<double>(duration_count)
            : static_cast<double>(duration_sum_us) /
                  static_cast<double>(duration_count);
    duration_mean_us = new_mean;
    const double delta2 = static_cast<double>(dur) - new_mean;
    duration_m2 += delta * delta2;

    double dur_d = static_cast<double>(dur);
    duration_sketch.add(dur_d);
    duration_histogram.add(dur);

    if (names.name_sketch == NO_SLOT) {
        names.name_sketch = slot_of(name_duration_sketches, name);
        names.name_histogram = slot_of(name_duration_histograms, name);
        names.name_sum = slot_of(name_duration_sums, name);
        names.name_sum_sq = slot_of(name_duration_sum_sqs, name);
        names.cat_sketch = slot_of(cat_duration_sketches, cat);
        names.cat_sum = slot_of(cat_duration_sums, cat);
    }
    at_slot(name_duration_sketches, names.name_sketch).add(dur_d);
    at_slot(name_duration_histograms, names.name_histogram).add(dur);
    at_slot(name_duration_sums, names.name_sum) += dur_d;
    at_slot(name_duration_sum_sqs, names.name_sum_sq) += dur_d * dur_d;
    at_slot(cat_duration_sketches, names.cat_sketch).add(dur_d);
    at_slot(cat_duration_sums, names.cat_sum) += dur_d;

    // Per-pid duration aggregates, keyed by the decimal pid so the key matches
    // the viz group-by-pid value.
    if (pids.pid_sketch == NO_SLOT) {
        char pid_buf[std::numeric_limits<std::uint64_t>::digits10 + 2];
        auto [pe, pec] =
            dstd::to_chars(pid_buf, pid_buf + sizeof(pid_buf), pid);
        if (pec != std::errc{}) return;
        const dstd::StringView pid_key(pid_buf,
                                       static_cast<std::size_t>(pe - pid_buf));
        pids.pid_sketch = slot_of(pid_duration_sketches, pid_key);
        pids.pid_sum = slot_of(pid_duration_sums, pid_key);
    }
    at_slot(pid_duration_sketches, pids.pid_sketch).add(dur_d);
    at_slot(pid_duration_sums, pids.pid_sum) += dur_d;
}

void ChunkStatistics::merge_from(const ChunkStatistics& other) {
    if (other.total_events == 0) return;

    for (const auto& [k, v] : other.category_counts) category_counts[k] += v;
    for (const auto& [k, v] : other.name_counts) name_counts[k] += v;
    for (const auto& [k, v] : other.pid_tid_counts) pid_tid_counts[k] += v;

    total_events += other.total_events;
    min_timestamp_us = std::min(min_timestamp_us, other.min_timestamp_us);
    min_nonzero_timestamp_us =
        std::min(min_nonzero_timestamp_us, other.min_nonzero_timestamp_us);
    max_timestamp_us = std::max(max_timestamp_us, other.max_timestamp_us);

    // Merge duration stats using parallel Welford's
    if (other.duration_count > 0 && duration_count > 0) {
        const double mean_a = duration_mean();
        const double mean_b = other.duration_mean();
        double delta = mean_b - mean_a;
        std::uint64_t combined_count = duration_count + other.duration_count;
        duration_mean_us =
            (mean_a * static_cast<double>(duration_count) +
             mean_b * static_cast<double>(other.duration_count)) /
            static_cast<double>(combined_count);
        duration_m2 = duration_m2 + other.duration_m2 +
                      delta * delta * static_cast<double>(duration_count) *
                          static_cast<double>(other.duration_count) /
                          static_cast<double>(combined_count);
        duration_count = combined_count;
    } else if (other.duration_count > 0) {
        duration_count = other.duration_count;
        duration_m2 = other.duration_m2;
        duration_mean_us = other.duration_mean();
    }

    duration_sum_us = saturating_duration_sum(
        duration_sum_us, static_cast<std::uint64_t>(other.duration_sum_us));
    duration_min_us = std::min(duration_min_us, other.duration_min_us);
    duration_max_us = std::max(duration_max_us, other.duration_max_us);

    duration_sketch.merge(other.duration_sketch);
    duration_histogram.merge(other.duration_histogram);
    timestamp_histogram.merge(other.timestamp_histogram);

    for (const auto& [k, v] : other.name_duration_sketches) {
        name_duration_sketches[k].merge(v);
    }
    for (const auto& [k, v] : other.name_duration_histograms) {
        name_duration_histograms[k].merge(v);
    }
    for (const auto& [k, v] : other.name_duration_sums) {
        name_duration_sums[k] += v;
    }
    for (const auto& [k, v] : other.name_duration_sum_sqs) {
        name_duration_sum_sqs[k] += v;
    }
    for (const auto& [k, v] : other.name_category) {
        name_category.emplace(k, v);
    }
    for (const auto& [k, v] : other.cat_duration_sketches) {
        cat_duration_sketches[k].merge(v);
    }
    for (const auto& [k, v] : other.cat_duration_sums) {
        cat_duration_sums[k] += v;
    }
    for (const auto& [k, v] : other.pid_duration_sketches) {
        pid_duration_sketches[k].merge(v);
    }
    for (const auto& [k, v] : other.pid_duration_sums) {
        pid_duration_sums[k] += v;
    }
}

std::int64_t saturating_duration_sum(std::int64_t sum, std::uint64_t add) {
    constexpr std::int64_t MAX = std::numeric_limits<std::int64_t>::max();
    const auto room = static_cast<std::uint64_t>(MAX - sum);
    return add > room ? MAX : sum + static_cast<std::int64_t>(add);
}

double ChunkStatistics::duration_mean() const {
    if (duration_count == 0) return 0.0;
    if (duration_sum_us == std::numeric_limits<std::int64_t>::max() &&
        duration_mean_us > 0.0)
        return duration_mean_us;
    return static_cast<double>(duration_sum_us) /
           static_cast<double>(duration_count);
}

double ChunkStatistics::duration_variance() const {
    if (duration_count < 2) return 0.0;
    return duration_m2 / static_cast<double>(duration_count - 1);
}

dstd::String ChunkStatistics::name_category_json() const {
    dstd::String out = "{";
    bool first = true;
    for (const auto& [key, value] : name_category) {
        if (!first) out += ',';
        first = false;
        out += '"';
        out += key;
        out += "\":\"";
        out += value;
        out += '"';
    }
    out += '}';
    return out;
}

dstd::StringViewMap<dstd::String> ChunkStatistics::parse_string_map_json(
    const dstd::String& json) {
    dstd::StringViewMap<dstd::String> result;

    dftracer::utils::json::RecordParser parser;
    auto parse_result = parser.parse(json.data(), json.size());
    if (parse_result.error()) return result;

    auto root = parse_result.value_unsafe();
    if (!root.is_object()) return result;

    auto obj = root.get_object().value_unsafe();
    for (auto field : obj) {
        auto val_result = field.value.get_string();
        if (!val_result.error()) {
            result[dstd::String(field.key)] =
                dstd::String(val_result.value_unsafe());
        }
    }
    return result;
}

dstd::String ChunkStatistics::name_duration_histograms_json() const {
    dstd::String out = "{";
    bool first = true;
    for (const auto& [key, hist] : name_duration_histograms) {
        if (!first) out += ',';
        first = false;
        out += '"';
        out += key;
        out += "\":";
        out += hist.to_json();
    }
    out += '}';
    return out;
}

namespace {
template <typename Map>
dstd::String double_map_to_json(const Map& map) {
    dstd::String out = "{";
    bool first = true;
    for (const auto& [key, value] : map) {
        if (!first) out += ',';
        first = false;
        // 17 significant digits, as the stored index text has always been.
        char buf[32];
        const int n = std::snprintf(buf, sizeof(buf), "%.17g", value);
        out += '"';
        out += key;
        out += "\":";
        out.append(buf, static_cast<std::size_t>(n));
    }
    out += '}';
    return out;
}
}  // namespace

dstd::String ChunkStatistics::name_duration_sums_json() const {
    return double_map_to_json(name_duration_sums);
}

dstd::String ChunkStatistics::name_duration_sum_sqs_json() const {
    return double_map_to_json(name_duration_sum_sqs);
}

dstd::String ChunkStatistics::cat_duration_sums_json() const {
    return double_map_to_json(cat_duration_sums);
}

dstd::String ChunkStatistics::pid_duration_sums_json() const {
    return double_map_to_json(pid_duration_sums);
}

dstd::Vector<std::uint8_t> ChunkStatistics::serialize_name_duration_sketches()
    const {
    return serialize_sketch_map(name_duration_sketches);
}

// Binary format: uint32_t num_entries, then per entry:
//   uint32_t key_len, char[key_len], uint32_t blob_len, uint8_t[blob_len]
dstd::Vector<std::uint8_t> ChunkStatistics::serialize_sketch_map(
    const dstd::StringViewMap<dataframe::DDSketch>& sketches) {
    dstd::Vector<std::uint8_t> buf;
    auto num = static_cast<std::uint32_t>(sketches.size());

    // NOTE(perf): pre-reserve header + estimated ~512 bytes per sketch entry
    buf.reserve(sizeof(std::uint32_t) + num * 512);

    dstd::append_u32_le(buf, num);

    dstd::Vector<std::uint8_t> sketch_blob;
    for (const auto& [key, sketch] : sketches) {
        sketch.serialize_into(sketch_blob);
        auto key_len = static_cast<std::uint32_t>(key.size());
        auto blob_len = static_cast<std::uint32_t>(sketch_blob.size());

        dstd::append_u32_le(buf, key_len);
        buf.insert(buf.end(), key.data(), key.data() + key_len);
        dstd::append_u32_le(buf, blob_len);
        buf.insert(buf.end(), sketch_blob.begin(), sketch_blob.end());
    }

    return buf;
}

dstd::StringViewMap<double> ChunkStatistics::parse_double_map_json(
    const dstd::String& json) {
    dstd::StringViewMap<double> result;

    dftracer::utils::json::RecordParser parser;
    auto parse_result = parser.parse(json.data(), json.size());
    if (parse_result.error()) return result;

    auto root = parse_result.value_unsafe();
    if (!root.is_object()) return result;

    auto obj = root.get_object().value_unsafe();
    for (auto field : obj) {
        auto double_result = field.value.get_double();
        if (!double_result.error()) {
            result[dstd::String(field.key)] = double_result.value_unsafe();
        } else {
            auto int_result = field.value.get_int64();
            if (!int_result.error()) {
                result[dstd::String(field.key)] =
                    static_cast<double>(int_result.value_unsafe());
            } else {
                auto uint_result = field.value.get_uint64();
                if (!uint_result.error()) {
                    result[dstd::String(field.key)] =
                        static_cast<double>(uint_result.value_unsafe());
                }
            }
        }
    }
    return result;
}

dstd::StringViewMap<dataframe::Log2Histogram>
ChunkStatistics::parse_histogram_map_json(const dstd::String& json) {
    dstd::StringViewMap<dataframe::Log2Histogram> result;

    dftracer::utils::json::RecordParser parser;
    auto parse_result = parser.parse(json.data(), json.size());
    if (parse_result.error()) return result;

    auto root = parse_result.value_unsafe();
    if (!root.is_object()) return result;

    auto obj = root.get_object().value_unsafe();
    for (auto field : obj) {
        if (!field.value.is_array()) continue;

        dataframe::Log2Histogram hist;
        auto arr = field.value.get_array().value_unsafe();
        for (auto pair : arr) {
            if (!pair.is_array()) continue;
            auto pair_arr = pair.get_array().value_unsafe();
            if (pair_arr.size() != 2) continue;

            auto bin_idx_result = pair_arr.at(0).get_uint64();
            auto count_result = pair_arr.at(1).get_uint64();
            if (bin_idx_result.error() || count_result.error()) continue;

            auto bin_idx =
                static_cast<std::size_t>(bin_idx_result.value_unsafe());
            auto count = count_result.value_unsafe();
            if (bin_idx < dataframe::Log2Histogram::NUM_BINS) {
                hist.add(dataframe::Log2Histogram::bin_lower(bin_idx), count);
            }
        }
        result[dstd::String(field.key)] = std::move(hist);
    }
    return result;
}

dstd::StringViewMap<dataframe::DDSketch>
ChunkStatistics::deserialize_name_duration_sketches(const std::uint8_t* data,
                                                    std::size_t len) {
    dstd::StringViewMap<dataframe::DDSketch> result;
    if (!data || len < sizeof(std::uint32_t)) return result;

    dstd::LeReader r(data, len);
    std::uint32_t num_entries = 0;
    r.u32(num_entries);

    for (std::uint32_t i = 0; i < num_entries && r.remaining() > 0; ++i) {
        std::uint32_t key_len = 0;
        if (!r.u32(key_len)) break;
        const std::uint8_t* key_bytes = r.take(key_len);
        if (!key_bytes) break;
        dstd::String key(reinterpret_cast<const char*>(key_bytes), key_len);

        std::uint32_t blob_len = 0;
        if (!r.u32(blob_len)) break;
        const std::uint8_t* blob = r.take(blob_len);
        if (!blob) break;
        result[key] = dataframe::DDSketch::deserialize(blob, blob_len);
    }

    return result;
}

}  // namespace dftracer::utils::index::schemas::dft
