#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/index/event_cost.h>
#include <dftracer/utils/index/schemas/dft/chunk_statistics.h>
#include <dftracer/utils/json/record_parser.h>
#include <simdjson.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace dftracer::utils::index::schemas::dft {

namespace {

using utilities::common::statistics::DDSketch;
using utilities::common::statistics::Log2Histogram;

template <class V>
std::uint32_t slot_of(StringViewMap<V>& map, std::string_view key) {
    auto it = map.find(key);
    if (it == map.end()) it = map.emplace(std::string(key), V{}).first;
    return static_cast<std::uint32_t>(it - map.begin());
}

template <class V>
V& at_slot(StringViewMap<V>& map, std::uint32_t slot) {
    return (map.begin() + slot)->second;
}

}  // namespace

void ChunkStatistics::update_from_event(std::string_view name,
                                        std::string_view cat, std::uint64_t pid,
                                        std::uint64_t tid, std::uint64_t ts,
                                        std::uint64_t dur, bool has_dur) {
    NameSlots names;
    PidSlots pids;
    update_from_event(names, pids, name, cat, pid, tid, ts, dur, has_dur);
}

void ChunkStatistics::update_from_event(NameSlots& names, PidSlots& pids,
                                        std::string_view name,
                                        std::string_view cat, std::uint64_t pid,
                                        std::uint64_t tid, std::uint64_t ts,
                                        std::uint64_t dur, bool has_dur) {
    ++total_events;

    if (names.category_count == NO_SLOT) {
        index::event_cost::g_stats_string_lookups.fetch_add(
            1, std::memory_order_relaxed);
        names.category_count = slot_of(category_counts, cat);
        names.name_count = slot_of(name_counts, name);
        // name -> category is a property of the name, recorded for every event.
        if (name_category.find(name) == name_category.end())
            name_category.emplace(std::string(name), cat);
    }
    if (pids.pid_tid_count == NO_SLOT) {
        constexpr std::size_t pid_tid_buf_size =
            (2 * std::numeric_limits<std::uint64_t>::digits10) + 3;
        char pt_buf[pid_tid_buf_size];
        auto [pp, ec1] = std::to_chars(pt_buf, pt_buf + sizeof(pt_buf), pid);
        if (ec1 != std::errc{} || pp == pt_buf + sizeof(pt_buf)) {
            throw DFTUtilsException(ErrorCode::Internal,
                                    "failed to format pid");
        }
        *pp++ = ':';
        auto [tp, ec2] = std::to_chars(pp, pt_buf + sizeof(pt_buf), tid);
        if (ec2 != std::errc{}) {
            throw DFTUtilsException(ErrorCode::Internal,
                                    "failed to format tid");
        }
        pids.pid_tid_count =
            slot_of(pid_tid_counts, std::string_view(pt_buf, tp - pt_buf));
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
    double old_mean = (duration_count > 0)
                          ? static_cast<double>(duration_sum_us) /
                                static_cast<double>(duration_count)
                          : 0.0;

    ++duration_count;
    duration_sum_us += static_cast<std::int64_t>(dur);
    if (dur < duration_min_us) duration_min_us = dur;
    if (dur > duration_max_us) duration_max_us = dur;

    double delta = static_cast<double>(dur) - old_mean;
    double new_mean = static_cast<double>(duration_sum_us) /
                      static_cast<double>(duration_count);
    double delta2 = static_cast<double>(dur) - new_mean;
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
        auto [pe, pec] = std::to_chars(pid_buf, pid_buf + sizeof(pid_buf), pid);
        if (pec != std::errc{}) return;
        const std::string_view pid_key(pid_buf,
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
        double mean_a = static_cast<double>(duration_sum_us) /
                        static_cast<double>(duration_count);
        double mean_b = static_cast<double>(other.duration_sum_us) /
                        static_cast<double>(other.duration_count);
        double delta = mean_b - mean_a;
        std::uint64_t combined_count = duration_count + other.duration_count;
        duration_m2 = duration_m2 + other.duration_m2 +
                      delta * delta * static_cast<double>(duration_count) *
                          static_cast<double>(other.duration_count) /
                          static_cast<double>(combined_count);
        duration_count = combined_count;
    } else if (other.duration_count > 0) {
        duration_count = other.duration_count;
        duration_m2 = other.duration_m2;
    }

    duration_sum_us += other.duration_sum_us;
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

double ChunkStatistics::duration_mean() const {
    if (duration_count == 0) return 0.0;
    return static_cast<double>(duration_sum_us) /
           static_cast<double>(duration_count);
}

double ChunkStatistics::duration_variance() const {
    if (duration_count < 2) return 0.0;
    return duration_m2 / static_cast<double>(duration_count - 1);
}

std::string ChunkStatistics::name_category_json() const {
    std::ostringstream ss;
    ss << '{';
    bool first = true;
    for (const auto& [key, value] : name_category) {
        if (!first) ss << ',';
        first = false;
        ss << '"' << key << "\":\"" << value << '"';
    }
    ss << '}';
    return ss.str();
}

StringViewMap<std::string> ChunkStatistics::parse_string_map_json(
    const std::string& json) {
    StringViewMap<std::string> result;

    dftracer::utils::json::RecordParser parser;
    auto parse_result = parser.parse(json.data(), json.size());
    if (parse_result.error()) return result;

    auto root = parse_result.value_unsafe();
    if (!root.is_object()) return result;

    auto obj = root.get_object().value_unsafe();
    for (auto field : obj) {
        auto val_result = field.value.get_string();
        if (!val_result.error()) {
            result[std::string(field.key)] =
                std::string(val_result.value_unsafe());
        }
    }
    return result;
}

std::string ChunkStatistics::name_duration_histograms_json() const {
    std::ostringstream ss;
    ss << '{';
    bool first = true;
    for (const auto& [key, hist] : name_duration_histograms) {
        if (!first) ss << ',';
        first = false;
        ss << '"' << key << "\":" << hist.to_json();
    }
    ss << '}';
    return ss.str();
}

namespace {
template <typename Map>
std::string double_map_to_json(const Map& map) {
    std::ostringstream ss;
    ss << std::setprecision(17) << '{';
    bool first = true;
    for (const auto& [key, value] : map) {
        if (!first) ss << ',';
        first = false;
        ss << '"' << key << "\":" << value;
    }
    ss << '}';
    return ss.str();
}
}  // namespace

std::string ChunkStatistics::name_duration_sums_json() const {
    return double_map_to_json(name_duration_sums);
}

std::string ChunkStatistics::name_duration_sum_sqs_json() const {
    return double_map_to_json(name_duration_sum_sqs);
}

std::string ChunkStatistics::cat_duration_sums_json() const {
    return double_map_to_json(cat_duration_sums);
}

std::string ChunkStatistics::pid_duration_sums_json() const {
    return double_map_to_json(pid_duration_sums);
}

std::vector<std::uint8_t> ChunkStatistics::serialize_name_duration_sketches()
    const {
    return serialize_sketch_map(name_duration_sketches);
}

// Binary format: uint32_t num_entries, then per entry:
//   uint32_t key_len, char[key_len], uint32_t blob_len, uint8_t[blob_len]
std::vector<std::uint8_t> ChunkStatistics::serialize_sketch_map(
    const StringViewMap<utilities::common::statistics::DDSketch>& sketches) {
    std::vector<std::uint8_t> buf;
    auto num = static_cast<std::uint32_t>(sketches.size());

    // NOTE(perf): pre-reserve header + estimated ~512 bytes per sketch entry
    buf.reserve(sizeof(std::uint32_t) + num * 512);

    append_u32_le(buf, num);

    std::vector<std::uint8_t> sketch_blob;
    for (const auto& [key, sketch] : sketches) {
        sketch.serialize_into(sketch_blob);
        auto key_len = static_cast<std::uint32_t>(key.size());
        auto blob_len = static_cast<std::uint32_t>(sketch_blob.size());

        append_u32_le(buf, key_len);
        buf.insert(buf.end(), key.data(), key.data() + key_len);
        append_u32_le(buf, blob_len);
        buf.insert(buf.end(), sketch_blob.begin(), sketch_blob.end());
    }

    return buf;
}

StringViewMap<double> ChunkStatistics::parse_double_map_json(
    const std::string& json) {
    StringViewMap<double> result;

    dftracer::utils::json::RecordParser parser;
    auto parse_result = parser.parse(json.data(), json.size());
    if (parse_result.error()) return result;

    auto root = parse_result.value_unsafe();
    if (!root.is_object()) return result;

    auto obj = root.get_object().value_unsafe();
    for (auto field : obj) {
        auto double_result = field.value.get_double();
        if (!double_result.error()) {
            result[std::string(field.key)] = double_result.value_unsafe();
        } else {
            auto int_result = field.value.get_int64();
            if (!int_result.error()) {
                result[std::string(field.key)] =
                    static_cast<double>(int_result.value_unsafe());
            } else {
                auto uint_result = field.value.get_uint64();
                if (!uint_result.error()) {
                    result[std::string(field.key)] =
                        static_cast<double>(uint_result.value_unsafe());
                }
            }
        }
    }
    return result;
}

StringViewMap<utilities::common::statistics::Log2Histogram>
ChunkStatistics::parse_histogram_map_json(const std::string& json) {
    StringViewMap<utilities::common::statistics::Log2Histogram> result;

    dftracer::utils::json::RecordParser parser;
    auto parse_result = parser.parse(json.data(), json.size());
    if (parse_result.error()) return result;

    auto root = parse_result.value_unsafe();
    if (!root.is_object()) return result;

    auto obj = root.get_object().value_unsafe();
    for (auto field : obj) {
        if (!field.value.is_array()) continue;

        utilities::common::statistics::Log2Histogram hist;
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
            if (bin_idx <
                utilities::common::statistics::Log2Histogram::NUM_BINS) {
                hist.add(
                    utilities::common::statistics::Log2Histogram::bin_lower(
                        bin_idx),
                    count);
            }
        }
        result[std::string(field.key)] = std::move(hist);
    }
    return result;
}

StringViewMap<utilities::common::statistics::DDSketch>
ChunkStatistics::deserialize_name_duration_sketches(const std::uint8_t* data,
                                                    std::size_t len) {
    StringViewMap<utilities::common::statistics::DDSketch> result;
    if (!data || len < sizeof(std::uint32_t)) return result;

    LeReader r(data, len);
    std::uint32_t num_entries = 0;
    r.u32(num_entries);

    for (std::uint32_t i = 0; i < num_entries && r.remaining() > 0; ++i) {
        std::uint32_t key_len = 0;
        if (!r.u32(key_len)) break;
        const std::uint8_t* key_bytes = r.take(key_len);
        if (!key_bytes) break;
        std::string key(reinterpret_cast<const char*>(key_bytes), key_len);

        std::uint32_t blob_len = 0;
        if (!r.u32(blob_len)) break;
        const std::uint8_t* blob = r.take(blob_len);
        if (!blob) break;
        result[key] = utilities::common::statistics::DDSketch::deserialize(
            blob, blob_len);
    }

    return result;
}

}  // namespace dftracer::utils::index::schemas::dft
