#include <dftracer/utils/utilities/common/statistics/ddsketch.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace dftracer::utils::utilities::common::statistics {

DDSketch::DDSketch(double relative_accuracy)
    : gamma_((1.0 + relative_accuracy) / (1.0 - relative_accuracy)),
      log_gamma_(std::log(gamma_)),
      min_(std::numeric_limits<double>::infinity()),
      max_(-std::numeric_limits<double>::infinity()),
      count_(0),
      zero_count_(0) {}

int DDSketch::bin_index(double value) const {
    return static_cast<int>(std::ceil(std::log(value) / log_gamma_));
}

double DDSketch::bin_lower_bound(int index) const {
    return std::pow(gamma_, index - 1);
}

double DDSketch::bin_upper_bound(int index) const {
    return std::pow(gamma_, index);
}

void DDSketch::add_to_bin(int index, uint64_t count) {
    auto it = std::lower_bound(bins_.begin(), bins_.end(), index,
                               [](const std::pair<int, uint64_t>& bin,
                                  int idx) { return bin.first < idx; });

    if (it != bins_.end() && it->first == index) {
        it->second += count;
    } else {
        bins_.insert(it, {index, count});
    }
}

void DDSketch::add(double value, double weight) {
    if (weight <= 0.0) return;

    uint64_t w = static_cast<uint64_t>(weight);
    if (w == 0) w = 1;

    if (value < min_) min_ = value;
    if (value > max_) max_ = value;

    count_ += w;

    if (value == 0.0) {
        zero_count_ += w;
        return;
    }

    // DDSketch handles positive values natively. For negative values,
    // we treat them as their absolute value -- appropriate since our
    // use case (durations, sizes) only produces non-negative values.
    double abs_value = std::abs(value);
    int idx = bin_index(abs_value);
    add_to_bin(idx, w);
}

void DDSketch::merge(const DDSketch& other) {
    if (other.count_ == 0) return;

    if (other.min_ < min_) min_ = other.min_;
    if (other.max_ > max_) max_ = other.max_;

    count_ += other.count_;
    zero_count_ += other.zero_count_;

    // Merge sorted bin vectors: walk both in order, sum matching indices.
    std::vector<std::pair<int, uint64_t>> merged;
    merged.reserve(bins_.size() + other.bins_.size());

    auto it1 = bins_.begin();
    auto it2 = other.bins_.begin();

    while (it1 != bins_.end() && it2 != other.bins_.end()) {
        if (it1->first < it2->first) {
            merged.push_back(*it1++);
        } else if (it1->first > it2->first) {
            merged.push_back(*it2++);
        } else {
            merged.push_back({it1->first, it1->second + it2->second});
            ++it1;
            ++it2;
        }
    }

    while (it1 != bins_.end()) merged.push_back(*it1++);
    while (it2 != other.bins_.end()) merged.push_back(*it2++);

    bins_ = std::move(merged);
}

double DDSketch::quantile(double q) const {
    if (count_ == 0) return std::numeric_limits<double>::quiet_NaN();
    if (q <= 0.0) return min_;
    if (q >= 1.0) return max_;

    double target_rank = q * static_cast<double>(count_);
    double cumulative = 0.0;

    // Account for zero-valued entries first
    cumulative += static_cast<double>(zero_count_);
    if (cumulative >= target_rank && zero_count_ > 0) {
        return 0.0;
    }

    // Walk bins returning bin midpoint for normal cases.
    // At gap boundaries (target rank near top of bin, next occupied bin is
    // non-adjacent), average the current bin's upper edge with the next
    // bin's lower edge -- analogous to standard quantile averaging of
    // boundary values in sorted data.
    for (std::size_t i = 0; i < bins_.size(); ++i) {
        auto [idx, bin_count] = bins_[i];
        cumulative += static_cast<double>(bin_count);

        if (cumulative >= target_rank) {
            double lower = bin_lower_bound(idx);
            double upper = bin_upper_bound(idx);

            if (cumulative - target_rank < 1.0 && i + 1 < bins_.size()) {
                int next_idx = bins_[i + 1].first;
                if (next_idx > idx + 1) {
                    double next_lower = bin_lower_bound(next_idx);
                    return (upper + next_lower) / 2.0;
                }
            }

            return (lower + upper) / 2.0;
        }
    }

    return max_;
}

void DDSketch::reset() {
    bins_.clear();
    min_ = std::numeric_limits<double>::infinity();
    max_ = -std::numeric_limits<double>::infinity();
    count_ = 0;
    zero_count_ = 0;
}

std::size_t DDSketch::memory_usage() const {
    return sizeof(DDSketch) +
           bins_.capacity() * sizeof(std::pair<int, uint64_t>);
}

std::vector<uint8_t> DDSketch::serialize() const {
    // Header: gamma(f64) min(f64) max(f64) count(u64) zero_count(u64)
    //         num_bins(u32)
    // Bins:   num_bins × (index(i32) count(u64))
    constexpr std::size_t HEADER_SIZE =
        sizeof(double) * 3 + sizeof(uint64_t) * 2 + sizeof(uint32_t);
    constexpr std::size_t BIN_SIZE = sizeof(int32_t) + sizeof(uint64_t);

    auto num_bins = static_cast<uint32_t>(bins_.size());
    std::vector<uint8_t> buf(HEADER_SIZE + num_bins * BIN_SIZE);
    uint8_t* p = buf.data();

    std::memcpy(p, &gamma_, sizeof(double));
    p += sizeof(double);
    std::memcpy(p, &min_, sizeof(double));
    p += sizeof(double);
    std::memcpy(p, &max_, sizeof(double));
    p += sizeof(double);
    std::memcpy(p, &count_, sizeof(uint64_t));
    p += sizeof(uint64_t);
    std::memcpy(p, &zero_count_, sizeof(uint64_t));
    p += sizeof(uint64_t);
    std::memcpy(p, &num_bins, sizeof(uint32_t));
    p += sizeof(uint32_t);

    for (const auto& [idx, cnt] : bins_) {
        auto idx32 = static_cast<int32_t>(idx);
        std::memcpy(p, &idx32, sizeof(int32_t));
        p += sizeof(int32_t);
        std::memcpy(p, &cnt, sizeof(uint64_t));
        p += sizeof(uint64_t);
    }

    return buf;
}

DDSketch DDSketch::deserialize(const uint8_t* data, std::size_t len) {
    constexpr std::size_t HEADER_SIZE =
        sizeof(double) * 3 + sizeof(uint64_t) * 2 + sizeof(uint32_t);
    constexpr std::size_t BIN_SIZE = sizeof(int32_t) + sizeof(uint64_t);

    if (len < HEADER_SIZE) {
        return DDSketch{};
    }

    DDSketch s;
    const uint8_t* p = data;

    std::memcpy(&s.gamma_, p, sizeof(double));
    p += sizeof(double);
    s.log_gamma_ = std::log(s.gamma_);
    std::memcpy(&s.min_, p, sizeof(double));
    p += sizeof(double);
    std::memcpy(&s.max_, p, sizeof(double));
    p += sizeof(double);
    std::memcpy(&s.count_, p, sizeof(uint64_t));
    p += sizeof(uint64_t);
    std::memcpy(&s.zero_count_, p, sizeof(uint64_t));
    p += sizeof(uint64_t);

    uint32_t num_bins = 0;
    std::memcpy(&num_bins, p, sizeof(uint32_t));
    p += sizeof(uint32_t);

    if (len < HEADER_SIZE + num_bins * BIN_SIZE) {
        return DDSketch{};
    }

    s.bins_.reserve(num_bins);
    for (uint32_t i = 0; i < num_bins; ++i) {
        int32_t idx = 0;
        uint64_t cnt = 0;
        std::memcpy(&idx, p, sizeof(int32_t));
        p += sizeof(int32_t);
        std::memcpy(&cnt, p, sizeof(uint64_t));
        p += sizeof(uint64_t);
        s.bins_.emplace_back(static_cast<int>(idx), cnt);
    }

    return s;
}

}  // namespace dftracer::utils::utilities::common::statistics
