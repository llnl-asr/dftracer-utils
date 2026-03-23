#include <dftracer/utils/utilities/composites/dft/indexing/chunk_statistics.h>
#include <yyjson.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace dftracer::utils::utilities::composites::dft::indexing {

void ChunkStatistics::update_from_event(std::string_view name,
                                        std::string_view cat, std::uint64_t pid,
                                        std::uint64_t tid, std::uint64_t ts,
                                        std::uint64_t dur) {
    ++total_events;

    category_counts[std::string(cat)]++;
    name_counts[std::string(name)]++;
    std::string pid_tid = std::to_string(pid) + ":" + std::to_string(tid);
    pid_tid_counts[pid_tid]++;

    if (ts < min_timestamp_us) min_timestamp_us = ts;
    std::uint64_t end_ts = ts + dur;
    if (end_ts > max_timestamp_us) max_timestamp_us = end_ts;

    // Welford's online algorithm for variance
    // Compute old_mean BEFORE updating count/sum
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

    std::string name_str(name);
    name_duration_sketches[name_str].add(dur_d);
    name_duration_histograms[name_str].add(dur);
    name_duration_sums[name_str] += dur_d;
    name_duration_sum_sqs[name_str] += dur_d * dur_d;
    name_category.emplace(name_str, std::string(cat));
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
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    for (const auto& [key, value] : name_category) {
        yyjson_mut_obj_add_str(doc, root, key.c_str(), value.c_str());
    }

    char* json_str = yyjson_mut_write(doc, YYJSON_WRITE_NOFLAG, nullptr);
    std::string result(json_str ? json_str : "{}");
    if (json_str) free(json_str);
    yyjson_mut_doc_free(doc);
    return result;
}

std::unordered_map<std::string, std::string>
ChunkStatistics::parse_string_map_json(const std::string& json) {
    std::unordered_map<std::string, std::string> result;

    yyjson_doc* doc =
        yyjson_read(json.c_str(), json.size(), YYJSON_READ_NOFLAG);
    if (!doc) return result;

    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return result;
    }

    yyjson_obj_iter iter;
    yyjson_obj_iter_init(root, &iter);
    yyjson_val* key;
    while ((key = yyjson_obj_iter_next(&iter))) {
        yyjson_val* val = yyjson_obj_iter_get_val(key);
        if (yyjson_is_str(val)) {
            result[yyjson_get_str(key)] = yyjson_get_str(val);
        }
    }

    yyjson_doc_free(doc);
    return result;
}

std::string ChunkStatistics::name_duration_histograms_json() const {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    for (const auto& [key, hist] : name_duration_histograms) {
        yyjson_mut_val* arr = hist.to_yyjson(doc);
        yyjson_mut_obj_add_val(doc, root, key.c_str(), arr);
    }

    char* json_str = yyjson_mut_write(doc, YYJSON_WRITE_NOFLAG, nullptr);
    std::string result(json_str ? json_str : "{}");
    if (json_str) free(json_str);
    yyjson_mut_doc_free(doc);
    return result;
}

namespace {
std::string double_map_to_json(
    const std::unordered_map<std::string, double>& map) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    for (const auto& [key, value] : map) {
        yyjson_mut_obj_add_real(doc, root, key.c_str(), value);
    }

    char* json_str = yyjson_mut_write(doc, YYJSON_WRITE_NOFLAG, nullptr);
    std::string result(json_str ? json_str : "{}");
    if (json_str) free(json_str);
    yyjson_mut_doc_free(doc);
    return result;
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
std::vector<uint8_t> ChunkStatistics::serialize_name_duration_sketches() const {
    std::vector<uint8_t> buf;
    auto num = static_cast<uint32_t>(name_duration_sketches.size());
    buf.resize(sizeof(uint32_t));
    std::memcpy(buf.data(), &num, sizeof(uint32_t));

    for (const auto& [key, sketch] : name_duration_sketches) {
        auto sketch_blob = sketch.serialize();
        auto key_len = static_cast<uint32_t>(key.size());
        auto blob_len = static_cast<uint32_t>(sketch_blob.size());

        std::size_t offset = buf.size();
        buf.resize(offset + sizeof(uint32_t) + key_len + sizeof(uint32_t) +
                   blob_len);
        uint8_t* p = buf.data() + offset;

        std::memcpy(p, &key_len, sizeof(uint32_t));
        p += sizeof(uint32_t);
        std::memcpy(p, key.data(), key_len);
        p += key_len;
        std::memcpy(p, &blob_len, sizeof(uint32_t));
        p += sizeof(uint32_t);
        std::memcpy(p, sketch_blob.data(), blob_len);
    }

    return buf;
}

std::unordered_map<std::string, double> ChunkStatistics::parse_double_map_json(
    const std::string& json) {
    std::unordered_map<std::string, double> result;

    yyjson_doc* doc =
        yyjson_read(json.c_str(), json.size(), YYJSON_READ_NOFLAG);
    if (!doc) return result;

    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return result;
    }

    yyjson_obj_iter iter;
    yyjson_obj_iter_init(root, &iter);
    yyjson_val* key;
    while ((key = yyjson_obj_iter_next(&iter))) {
        yyjson_val* val = yyjson_obj_iter_get_val(key);
        if (yyjson_is_real(val)) {
            result[yyjson_get_str(key)] = yyjson_get_real(val);
        } else if (yyjson_is_int(val)) {
            result[yyjson_get_str(key)] =
                static_cast<double>(yyjson_get_int(val));
        } else if (yyjson_is_uint(val)) {
            result[yyjson_get_str(key)] =
                static_cast<double>(yyjson_get_uint(val));
        }
    }

    yyjson_doc_free(doc);
    return result;
}

std::unordered_map<std::string, common::statistics::Log2Histogram>
ChunkStatistics::parse_histogram_map_json(const std::string& json) {
    std::unordered_map<std::string, common::statistics::Log2Histogram> result;

    yyjson_doc* doc =
        yyjson_read(json.c_str(), json.size(), YYJSON_READ_NOFLAG);
    if (!doc) return result;

    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return result;
    }

    yyjson_obj_iter iter;
    yyjson_obj_iter_init(root, &iter);
    yyjson_val* key;
    while ((key = yyjson_obj_iter_next(&iter))) {
        yyjson_val* val = yyjson_obj_iter_get_val(key);
        if (!yyjson_is_arr(val)) continue;

        common::statistics::Log2Histogram hist;
        std::size_t idx, max;
        yyjson_val* pair;
        yyjson_arr_foreach(val, idx, max, pair) {
            if (!yyjson_is_arr(pair) || yyjson_arr_size(pair) != 2) continue;
            yyjson_val* bin_idx_val = yyjson_arr_get(pair, 0);
            yyjson_val* count_val = yyjson_arr_get(pair, 1);
            if (!yyjson_is_uint(bin_idx_val) || !yyjson_is_uint(count_val))
                continue;
            auto bin_idx =
                static_cast<std::size_t>(yyjson_get_uint(bin_idx_val));
            auto count = yyjson_get_uint(count_val);
            if (bin_idx < common::statistics::Log2Histogram::NUM_BINS) {
                hist.add(common::statistics::Log2Histogram::bin_lower(bin_idx),
                         count);
            }
        }
        result[yyjson_get_str(key)] = std::move(hist);
    }

    yyjson_doc_free(doc);
    return result;
}

std::unordered_map<std::string, common::statistics::DDSketch>
ChunkStatistics::deserialize_name_duration_sketches(const uint8_t* data,
                                                    std::size_t len) {
    std::unordered_map<std::string, common::statistics::DDSketch> result;
    if (!data || len < sizeof(uint32_t)) return result;

    const uint8_t* p = data;
    const uint8_t* end = data + len;

    uint32_t num_entries = 0;
    std::memcpy(&num_entries, p, sizeof(uint32_t));
    p += sizeof(uint32_t);

    for (uint32_t i = 0; i < num_entries && p < end; ++i) {
        if (p + sizeof(uint32_t) > end) break;
        uint32_t key_len = 0;
        std::memcpy(&key_len, p, sizeof(uint32_t));
        p += sizeof(uint32_t);

        if (p + key_len + sizeof(uint32_t) > end) break;
        std::string key(reinterpret_cast<const char*>(p), key_len);
        p += key_len;

        uint32_t blob_len = 0;
        std::memcpy(&blob_len, p, sizeof(uint32_t));
        p += sizeof(uint32_t);

        if (p + blob_len > end) break;
        result[key] = common::statistics::DDSketch::deserialize(p, blob_len);
        p += blob_len;
    }

    return result;
}

}  // namespace dftracer::utils::utilities::composites::dft::indexing
