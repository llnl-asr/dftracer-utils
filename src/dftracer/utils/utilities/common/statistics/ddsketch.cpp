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
      zero_count_(0) {
    store_.fill(0);
}

int DDSketch::bin_index(double value) const {
    return static_cast<int>(std::ceil(std::log(value) / log_gamma_));
}

double DDSketch::bin_lower_bound(int index) const {
    return std::pow(gamma_, index - 1);
}

double DDSketch::bin_upper_bound(int index) const {
    return std::pow(gamma_, index);
}

void DDSketch::add_to_bin(int index, std::uint16_t count) {
    if (!initialized_) {
        offset_ = index;
        min_key_ = index;
        max_key_ = index;
        store_[0] = count;
        num_bins_ = 1;
        initialized_ = true;
        return;
    }

    if (index >= min_key_ && index <= max_key_) {
        int pos = index - offset_;
        if (pos >= 0 && pos < MAX_BINS) {
            std::uint16_t old = store_[pos];
            store_[pos] = (old > UINT16_MAX - count) ? UINT16_MAX : old + count;
        }
        return;
    }

    if (index > max_key_) {
        int needed = index - min_key_ + 1;
        if (needed > MAX_BINS) {
            collapse_to_fit(index);
        }
        max_key_ = index;
        num_bins_ = max_key_ - min_key_ + 1;
        int pos = index - offset_;
        if (pos >= 0 && pos < MAX_BINS) {
            std::uint16_t old = store_[pos];
            store_[pos] = (old > UINT16_MAX - count) ? UINT16_MAX : old + count;
        }
        return;
    }

    // index < min_key_
    if (collapsed_) {
        std::uint16_t old = store_[0];
        store_[0] = (old > UINT16_MAX - count) ? UINT16_MAX : old + count;
        return;
    }

    int needed = max_key_ - index + 1;
    if (needed > MAX_BINS) {
        std::uint16_t old = store_[0];
        store_[0] = (old > UINT16_MAX - count) ? UINT16_MAX : old + count;
        collapsed_ = true;
        return;
    }

    int shift = min_key_ - index;
    if (shift > 0 && num_bins_ > 0) {
        std::memmove(
            &store_[shift], &store_[0],
            static_cast<std::size_t>(std::min(num_bins_, MAX_BINS - shift)) *
                sizeof(std::uint16_t));
        std::memset(&store_[0], 0,
                    static_cast<std::size_t>(shift) * sizeof(std::uint16_t));
    }
    offset_ = index;
    min_key_ = index;
    num_bins_ = max_key_ - min_key_ + 1;
    store_[0] = count;
}

void DDSketch::collapse_to_fit(int new_max_key) {
    int new_min_key = new_max_key - MAX_BINS + 1;

    if (new_min_key >= max_key_) {
        std::uint16_t total = 0;
        for (int i = 0; i < num_bins_ && i < MAX_BINS; ++i) {
            total = (total > UINT16_MAX - store_[i]) ? UINT16_MAX
                                                     : total + store_[i];
        }
        store_.fill(0);
        store_[0] = total;
        offset_ = new_min_key;
        min_key_ = new_min_key;
        max_key_ = new_max_key;
        num_bins_ = MAX_BINS;
        collapsed_ = true;
        return;
    }

    int collapse_count_bins = new_min_key - min_key_;
    if (collapse_count_bins > 0) {
        // Collapse bins below new_min_key into bin[0]
        std::uint16_t collapsed = 0;
        for (int i = 0; i < collapse_count_bins && i < MAX_BINS; ++i) {
            collapsed = (collapsed > UINT16_MAX - store_[i])
                            ? UINT16_MAX
                            : collapsed + store_[i];
            store_[i] = 0;
        }

        int remaining = num_bins_ - collapse_count_bins;
        if (remaining > 0 && collapse_count_bins > 0) {
            std::memmove(
                &store_[0], &store_[collapse_count_bins],
                static_cast<std::size_t>(std::min(remaining, MAX_BINS)) *
                    sizeof(std::uint16_t));
            int clear_start = std::min(remaining, MAX_BINS);
            int clear_count =
                std::min(collapse_count_bins, MAX_BINS - clear_start);
            if (clear_count > 0) {
                std::memset(&store_[clear_start], 0,
                            static_cast<std::size_t>(clear_count) *
                                sizeof(std::uint16_t));
            }
        }

        store_[0] = (store_[0] > UINT16_MAX - collapsed)
                        ? UINT16_MAX
                        : store_[0] + collapsed;
        offset_ = new_min_key;
        min_key_ = new_min_key;
    } else {
        // new_min_key <= min_key_: data needs to shift right to make
        // room for the extended range below
        int shift = min_key_ - new_min_key;
        if (shift > 0 && num_bins_ > 0) {
            int to_move = std::min(num_bins_, MAX_BINS - shift);
            if (to_move > 0) {
                std::memmove(
                    &store_[shift], &store_[0],
                    static_cast<std::size_t>(to_move) * sizeof(std::uint16_t));
            }
            std::memset(
                &store_[0], 0,
                static_cast<std::size_t>(shift) * sizeof(std::uint16_t));
        }
        offset_ = new_min_key;
        min_key_ = new_min_key;
    }

    max_key_ = new_max_key;
    num_bins_ = std::min(max_key_ - min_key_ + 1, MAX_BINS);
    collapsed_ = true;
}

void DDSketch::add(double value, double weight) {
    if (weight <= 0.0) return;

    std::uint16_t w = static_cast<std::uint16_t>(
        std::min(weight, static_cast<double>(UINT16_MAX)));
    if (w == 0) w = 1;

    if (value < min_) min_ = value;
    if (value > max_) max_ = value;

    count_ += w;

    if (value == 0.0) {
        zero_count_ += w;
        return;
    }

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

    if (!other.initialized_) return;

    if (!initialized_) {
        store_ = other.store_;
        offset_ = other.offset_;
        min_key_ = other.min_key_;
        max_key_ = other.max_key_;
        num_bins_ = other.num_bins_;
        initialized_ = other.initialized_;
        collapsed_ = other.collapsed_;
        return;
    }

    int merged_min = std::min(min_key_, other.min_key_);
    int merged_max = std::max(max_key_, other.max_key_);

    if (merged_max - merged_min + 1 > MAX_BINS) {
        collapse_to_fit(merged_max);
        merged_min = min_key_;
    }

    if (merged_min < min_key_) {
        int shift = min_key_ - merged_min;
        if (shift > 0 && num_bins_ > 0) {
            int to_move = std::min(num_bins_, MAX_BINS - shift);
            if (to_move > 0) {
                std::memmove(
                    &store_[shift], &store_[0],
                    static_cast<std::size_t>(to_move) * sizeof(std::uint16_t));
            }
            std::memset(
                &store_[0], 0,
                static_cast<std::size_t>(shift) * sizeof(std::uint16_t));
        }
        offset_ = merged_min;
        min_key_ = merged_min;
    }

    if (merged_max > max_key_) {
        max_key_ = merged_max;
    }
    num_bins_ = std::min(max_key_ - min_key_ + 1, MAX_BINS);

    for (int k = other.min_key_; k <= other.max_key_; ++k) {
        int other_pos = k - other.offset_;
        if (other_pos < 0 || other_pos >= MAX_BINS) continue;
        std::uint16_t v = other.store_[other_pos];
        if (v == 0) continue;

        int my_pos = k - offset_;
        if (my_pos < 0) {
            store_[0] =
                (store_[0] > UINT16_MAX - v) ? UINT16_MAX : store_[0] + v;
        } else if (my_pos < MAX_BINS) {
            store_[my_pos] = (store_[my_pos] > UINT16_MAX - v)
                                 ? UINT16_MAX
                                 : store_[my_pos] + v;
        }
    }
}

double DDSketch::quantile(double q) const {
    if (count_ == 0) return std::numeric_limits<double>::quiet_NaN();
    if (q <= 0.0) return min_;
    if (q >= 1.0) return max_;

    double target_rank = q * static_cast<double>(count_);
    double cumulative = static_cast<double>(zero_count_);
    if (cumulative >= target_rank && zero_count_ > 0) {
        return 0.0;
    }

    if (!initialized_) return 0.0;

    for (int i = 0; i < num_bins_ && i < MAX_BINS; ++i) {
        if (store_[i] == 0) continue;
        cumulative += static_cast<double>(store_[i]);

        if (cumulative >= target_rank) {
            int idx = i + offset_;
            double lower = bin_lower_bound(idx);
            double upper = bin_upper_bound(idx);
            return (lower + upper) / 2.0;
        }
    }

    return max_;
}

void DDSketch::reset() {
    store_.fill(0);
    offset_ = 0;
    min_key_ = 0;
    max_key_ = 0;
    num_bins_ = 0;
    initialized_ = false;
    collapsed_ = false;
    min_ = std::numeric_limits<double>::infinity();
    max_ = -std::numeric_limits<double>::infinity();
    count_ = 0;
    zero_count_ = 0;
}

std::size_t DDSketch::memory_usage() const { return sizeof(DDSketch); }

std::vector<std::uint8_t> DDSketch::serialize() const {
    std::vector<std::uint8_t> buf;
    serialize_into(buf);
    return buf;
}

void DDSketch::serialize_into(std::vector<std::uint8_t>& buf) const {
    constexpr std::size_t HEADER_SIZE =
        sizeof(double) * 3 + sizeof(std::uint64_t) * 2 + sizeof(std::int32_t) +
        sizeof(std::uint16_t);

    auto num = static_cast<std::uint32_t>(
        std::min(num_bins_, static_cast<int>(MAX_BINS)));
    std::size_t total = HEADER_SIZE + num * sizeof(std::uint16_t);
    buf.resize(total);
    std::uint8_t* p = buf.data();

    std::memcpy(p, &gamma_, sizeof(double));
    p += sizeof(double);
    std::memcpy(p, &min_, sizeof(double));
    p += sizeof(double);
    std::memcpy(p, &max_, sizeof(double));
    p += sizeof(double);
    std::memcpy(p, &count_, sizeof(std::uint64_t));
    p += sizeof(std::uint64_t);
    std::memcpy(p, &zero_count_, sizeof(std::uint64_t));
    p += sizeof(std::uint64_t);

    auto off32 = static_cast<std::int32_t>(offset_);
    std::memcpy(p, &off32, sizeof(std::int32_t));
    p += sizeof(std::int32_t);
    std::memcpy(p, &num, sizeof(std::uint16_t));
    p += sizeof(std::uint16_t);

    std::memcpy(p, store_.data(), num * sizeof(std::uint16_t));
}

DDSketch DDSketch::deserialize(const std::uint8_t* data, std::size_t len) {
    constexpr std::size_t HEADER_SIZE =
        sizeof(double) * 3 + sizeof(std::uint64_t) * 2 + sizeof(std::int32_t) +
        sizeof(std::uint16_t);

    if (len < HEADER_SIZE) {
        return DDSketch{};
    }

    DDSketch s;
    const std::uint8_t* p = data;

    std::memcpy(&s.gamma_, p, sizeof(double));
    p += sizeof(double);
    s.log_gamma_ = std::log(s.gamma_);
    std::memcpy(&s.min_, p, sizeof(double));
    p += sizeof(double);
    std::memcpy(&s.max_, p, sizeof(double));
    p += sizeof(double);
    std::memcpy(&s.count_, p, sizeof(std::uint64_t));
    p += sizeof(std::uint64_t);
    std::memcpy(&s.zero_count_, p, sizeof(std::uint64_t));
    p += sizeof(std::uint64_t);

    std::int32_t off32 = 0;
    std::memcpy(&off32, p, sizeof(std::int32_t));
    p += sizeof(std::int32_t);
    s.offset_ = static_cast<int>(off32);

    std::uint32_t num = 0;
    std::memcpy(&num, p, sizeof(std::uint16_t));
    p += sizeof(std::uint16_t);

    if (num > MAX_BINS) num = MAX_BINS;
    if (len < HEADER_SIZE + num * sizeof(std::uint16_t)) {
        return DDSketch{};
    }

    s.store_.fill(0);
    std::memcpy(s.store_.data(), p, num * sizeof(std::uint16_t));
    s.min_key_ = s.offset_;
    s.max_key_ = s.offset_ + static_cast<int>(num) - 1;
    s.num_bins_ = static_cast<int>(num);
    s.initialized_ = s.count_ > 0;

    return s;
}

}  // namespace dftracer::utils::utilities::common::statistics
