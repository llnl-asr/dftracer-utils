#include <dftracer/utils/utilities/common/statistics/timestamp_histogram.h>

#include <algorithm>
#include <cstring>

namespace dftracer::utils::utilities::common::statistics {

void TimestampHistogram::add(std::uint64_t timestamp_us) {
    std::uint64_t idx = bin_index(timestamp_us);
    total_count_++;

    auto it = std::lower_bound(
        bins_.begin(), bins_.end(), idx,
        [](const auto& p, std::uint64_t val) { return p.first < val; });

    if (it != bins_.end() && it->first == idx) {
        it->second++;
    } else {
        bins_.insert(it, {idx, 1});
    }
}

void TimestampHistogram::merge(const TimestampHistogram& other) {
    if (other.bins_.empty()) return;

    std::vector<std::pair<std::uint64_t, std::uint64_t>> merged;
    merged.reserve(bins_.size() + other.bins_.size());

    auto a = bins_.begin();
    auto b = other.bins_.begin();

    while (a != bins_.end() && b != other.bins_.end()) {
        if (a->first < b->first) {
            merged.push_back(*a++);
        } else if (a->first > b->first) {
            merged.push_back(*b++);
        } else {
            merged.push_back({a->first, a->second + b->second});
            ++a;
            ++b;
        }
    }
    while (a != bins_.end()) merged.push_back(*a++);
    while (b != other.bins_.end()) merged.push_back(*b++);

    bins_ = std::move(merged);
    total_count_ += other.total_count_;
}

std::uint64_t TimestampHistogram::count_in_range(
    std::uint64_t ts_start_us, std::uint64_t ts_end_us) const {
    if (bins_.empty() || ts_start_us >= ts_end_us) return 0;

    std::uint64_t start_bin = bin_index(ts_start_us);
    std::uint64_t end_bin = bin_index(ts_end_us - 1);

    auto it = std::lower_bound(
        bins_.begin(), bins_.end(), start_bin,
        [](const auto& p, std::uint64_t val) { return p.first < val; });

    std::uint64_t count = 0;
    for (; it != bins_.end() && it->first <= end_bin; ++it) {
        count += it->second;
    }
    return count;
}

double TimestampHistogram::selectivity(std::uint64_t ts_start_us,
                                       std::uint64_t ts_end_us) const {
    if (total_count_ == 0) return 0.0;
    return static_cast<double>(count_in_range(ts_start_us, ts_end_us)) /
           static_cast<double>(total_count_);
}

std::vector<double> TimestampHistogram::expansion_weights(
    std::uint64_t bucket_start_us, std::uint64_t bucket_end_us,
    std::size_t num_sub_buckets) const {
    std::vector<double> weights(num_sub_buckets, 0.0);
    if (num_sub_buckets == 0 || bucket_start_us >= bucket_end_us)
        return weights;

    std::uint64_t sub_width =
        (bucket_end_us - bucket_start_us) / num_sub_buckets;
    if (sub_width == 0) sub_width = 1;

    std::uint64_t total_in_range = 0;
    for (std::size_t i = 0; i < num_sub_buckets; ++i) {
        std::uint64_t sub_start = bucket_start_us + i * sub_width;
        std::uint64_t sub_end = (i + 1 < num_sub_buckets)
                                    ? bucket_start_us + (i + 1) * sub_width
                                    : bucket_end_us;
        std::uint64_t c = count_in_range(sub_start, sub_end);
        weights[i] = static_cast<double>(c);
        total_in_range += c;
    }

    if (total_in_range > 0) {
        double inv = 1.0 / static_cast<double>(total_in_range);
        for (auto& w : weights) w *= inv;
    } else {
        double uniform = 1.0 / static_cast<double>(num_sub_buckets);
        for (auto& w : weights) w = uniform;
    }

    return weights;
}

// Varint encoding helpers
namespace {

void encode_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(value | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

std::uint64_t decode_varint(const std::uint8_t*& ptr, const std::uint8_t* end) {
    std::uint64_t result = 0;
    unsigned shift = 0;
    while (ptr < end) {
        std::uint8_t byte = *ptr++;
        result |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) return result;
        shift += 7;
    }
    return result;
}

}  // namespace

std::vector<std::uint8_t> TimestampHistogram::serialize() const {
    std::vector<std::uint8_t> out;
    out.reserve(bins_.size() * 6 + 16);

    encode_varint(out, total_count_);
    encode_varint(out, bins_.size());

    std::uint64_t prev_idx = 0;
    for (const auto& [idx, count] : bins_) {
        encode_varint(out, idx - prev_idx);
        encode_varint(out, count);
        prev_idx = idx;
    }

    return out;
}

TimestampHistogram TimestampHistogram::deserialize(const std::uint8_t* data,
                                                   std::size_t len) {
    TimestampHistogram hist;
    if (!data || len == 0) return hist;

    const auto* ptr = data;
    const auto* end = data + len;

    hist.total_count_ = decode_varint(ptr, end);
    std::uint64_t num_bins = decode_varint(ptr, end);

    hist.bins_.reserve(static_cast<std::size_t>(num_bins));
    std::uint64_t prev_idx = 0;
    for (std::uint64_t i = 0; i < num_bins && ptr < end; ++i) {
        std::uint64_t delta = decode_varint(ptr, end);
        std::uint64_t count = decode_varint(ptr, end);
        prev_idx += delta;
        hist.bins_.push_back({prev_idx, count});
    }

    return hist;
}

}  // namespace dftracer::utils::utilities::common::statistics
