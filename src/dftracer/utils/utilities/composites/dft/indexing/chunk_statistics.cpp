#include <dftracer/utils/utilities/composites/dft/indexing/chunk_statistics.h>
#include <simdjson.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace dftracer::utils::utilities::composites::dft::indexing {

void ChunkStatistics::update_from_event(std::string_view name,
                                        std::string_view cat, std::uint64_t pid,
                                        std::uint64_t tid, std::uint64_t ts,
                                        std::uint64_t dur) {
    ++total_events;

    constexpr std::size_t pid_tid_buf_size =
        (2 * std::numeric_limits<std::uint64_t>::digits10) + 3;
    char pt_buf[pid_tid_buf_size];
    auto [pp, ec1] = std::to_chars(pt_buf, pt_buf + sizeof(pt_buf), pid);
    if (ec1 != std::errc{} || pp == pt_buf + sizeof(pt_buf)) {
        throw std::runtime_error("failed to format pid");
    }
    *pp++ = ':';
    auto [tp, ec2] = std::to_chars(pp, pt_buf + sizeof(pt_buf), tid);
    if (ec2 != std::errc{}) {
        throw std::runtime_error("failed to format tid");
    }
    std::string_view pt_sv(pt_buf, tp - pt_buf);

    // Increment counts with a single lookup — allocate a string only on
    // first observation.
    auto bump = [](auto& map, std::string_view key) {
        auto it = map.find(key);
        if (it == map.end()) {
            map.emplace(std::string(key), 1);
        } else {
            it->second++;
        }
    };
    bump(category_counts, cat);
    bump(name_counts, name);
    bump(pid_tid_counts, pt_sv);

    if (ts < min_timestamp_us) min_timestamp_us = ts;
    std::uint64_t end_ts = ts + dur;
    if (end_ts > max_timestamp_us) max_timestamp_us = end_ts;

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
    timestamp_histogram.add(ts);

    // name_duration_sketches: transparent find, allocate only on first
    // observation, then reuse the interned key for the other name_*_ maps.
    auto sketch_it = name_duration_sketches.find(name);
    if (sketch_it == name_duration_sketches.end()) {
        auto [new_it, _] = name_duration_sketches.emplace(
            std::string(name), common::statistics::DDSketch{});
        sketch_it = new_it;
    }
    sketch_it->second.add(dur_d);

    const std::string& name_key = sketch_it->first;
    name_duration_histograms[name_key].add(dur);
    name_duration_sums[name_key] += dur_d;
    name_duration_sum_sqs[name_key] += dur_d * dur_d;
    name_category.try_emplace(name_key, cat);
}

void ChunkStatistics::merge_from(const ChunkStatistics& other) {
    if (other.total_events == 0) return;

    for (const auto& [k, v] : other.category_counts) category_counts[k] += v;
    for (const auto& [k, v] : other.name_counts) name_counts[k] += v;
    for (const auto& [k, v] : other.pid_tid_counts) pid_tid_counts[k] += v;

    total_events += other.total_events;
    min_timestamp_us = std::min(min_timestamp_us, other.min_timestamp_us);
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

    simdjson::dom::parser parser;
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

// Binary format: uint32_t num_entries, then per entry:
//   uint32_t key_len, char[key_len], uint32_t blob_len, uint8_t[blob_len]
std::vector<std::uint8_t> ChunkStatistics::serialize_name_duration_sketches()
    const {
    std::vector<std::uint8_t> buf;
    auto num = static_cast<std::uint32_t>(name_duration_sketches.size());

    // NOTE(perf): pre-reserve header + estimated ~512 bytes per sketch entry
    buf.reserve(sizeof(std::uint32_t) + num * 512);

    buf.resize(sizeof(std::uint32_t));
    std::memcpy(buf.data(), &num, sizeof(std::uint32_t));

    std::vector<std::uint8_t> sketch_blob;
    for (const auto& [key, sketch] : name_duration_sketches) {
        sketch.serialize_into(sketch_blob);
        auto key_len = static_cast<std::uint32_t>(key.size());
        auto blob_len = static_cast<std::uint32_t>(sketch_blob.size());

        std::size_t offset = buf.size();
        buf.resize(offset + sizeof(std::uint32_t) + key_len +
                   sizeof(std::uint32_t) + blob_len);
        std::uint8_t* p = buf.data() + offset;

        std::memcpy(p, &key_len, sizeof(std::uint32_t));
        p += sizeof(std::uint32_t);
        std::memcpy(p, key.data(), key_len);
        p += key_len;
        std::memcpy(p, &blob_len, sizeof(std::uint32_t));
        p += sizeof(std::uint32_t);
        std::memcpy(p, sketch_blob.data(), blob_len);
    }

    return buf;
}

StringViewMap<double> ChunkStatistics::parse_double_map_json(
    const std::string& json) {
    StringViewMap<double> result;

    simdjson::dom::parser parser;
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

StringViewMap<common::statistics::Log2Histogram>
ChunkStatistics::parse_histogram_map_json(const std::string& json) {
    StringViewMap<common::statistics::Log2Histogram> result;

    simdjson::dom::parser parser;
    auto parse_result = parser.parse(json.data(), json.size());
    if (parse_result.error()) return result;

    auto root = parse_result.value_unsafe();
    if (!root.is_object()) return result;

    auto obj = root.get_object().value_unsafe();
    for (auto field : obj) {
        if (!field.value.is_array()) continue;

        common::statistics::Log2Histogram hist;
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
            if (bin_idx < common::statistics::Log2Histogram::NUM_BINS) {
                hist.add(common::statistics::Log2Histogram::bin_lower(bin_idx),
                         count);
            }
        }
        result[std::string(field.key)] = std::move(hist);
    }
    return result;
}

StringViewMap<common::statistics::DDSketch>
ChunkStatistics::deserialize_name_duration_sketches(const std::uint8_t* data,
                                                    std::size_t len) {
    StringViewMap<common::statistics::DDSketch> result;
    if (!data || len < sizeof(std::uint32_t)) return result;

    const std::uint8_t* p = data;
    const std::uint8_t* end = data + len;

    std::uint32_t num_entries = 0;
    std::memcpy(&num_entries, p, sizeof(std::uint32_t));
    p += sizeof(std::uint32_t);

    for (std::uint32_t i = 0; i < num_entries && p < end; ++i) {
        if (p + sizeof(std::uint32_t) > end) break;
        std::uint32_t key_len = 0;
        std::memcpy(&key_len, p, sizeof(std::uint32_t));
        p += sizeof(std::uint32_t);

        if (p + key_len + sizeof(std::uint32_t) > end) break;
        std::string key(reinterpret_cast<const char*>(p), key_len);
        p += key_len;

        std::uint32_t blob_len = 0;
        std::memcpy(&blob_len, p, sizeof(std::uint32_t));
        p += sizeof(std::uint32_t);

        if (p + blob_len > end) break;
        result[key] = common::statistics::DDSketch::deserialize(p, blob_len);
        p += blob_len;
    }

    return result;
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing
