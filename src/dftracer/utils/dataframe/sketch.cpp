#include <dftracer/utils/dataframe/sketch.h>
#include <dftracer/utils/dstd/little_endian.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace dftracer::utils::dataframe {

template <int BINS>
BasicDDSketch<BINS>::BasicDDSketch(double relative_accuracy)
    : gamma_((1.0 + relative_accuracy) / (1.0 - relative_accuracy)),
      log_gamma_(std::log(gamma_)),
      min_(std::numeric_limits<double>::infinity()),
      max_(-std::numeric_limits<double>::infinity()),
      count_(0),
      zero_count_(0) {}

template <int BINS>
double BasicDDSketch<BINS>::bin_lower_bound(int index) const {
    return std::pow(gamma_, index - 1);
}

template <int BINS>
double BasicDDSketch<BINS>::bin_upper_bound(int index) const {
    return std::pow(gamma_, index);
}

template <int BINS>
void BasicDDSketch<BINS>::add_to_bin(int index, std::uint32_t count) {
    if (!initialized_) {
        ensure_bins(1);
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
            std::uint32_t old = store_[pos];
            store_[pos] = (old > UINT32_MAX - count) ? UINT32_MAX : old + count;
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
            ensure_bins(pos + 1);
            std::uint32_t old = store_[pos];
            store_[pos] = (old > UINT32_MAX - count) ? UINT32_MAX : old + count;
        }
        return;
    }

    // index < min_key_
    if (collapsed_) {
        std::uint32_t old = store_[0];
        store_[0] = (old > UINT32_MAX - count) ? UINT32_MAX : old + count;
        return;
    }

    int needed = max_key_ - index + 1;
    if (needed > MAX_BINS) {
        std::uint32_t old = store_[0];
        store_[0] = (old > UINT32_MAX - count) ? UINT32_MAX : old + count;
        collapsed_ = true;
        return;
    }

    int shift = min_key_ - index;
    ensure_bins(needed);
    if (shift > 0 && num_bins_ > 0) {
        std::memmove(
            &store_[shift], &store_[0],
            static_cast<std::size_t>(std::min(num_bins_, MAX_BINS - shift)) *
                sizeof(std::uint32_t));
        std::memset(&store_[0], 0,
                    static_cast<std::size_t>(shift) * sizeof(std::uint32_t));
    }
    offset_ = index;
    min_key_ = index;
    num_bins_ = max_key_ - min_key_ + 1;
    store_[0] = count;
}

template <int BINS>
void BasicDDSketch<BINS>::collapse_to_fit(int new_max_key) {
    int new_min_key = new_max_key - MAX_BINS + 1;
    ensure_bins(MAX_BINS);

    if (new_min_key >= max_key_) {
        std::uint32_t total = 0;
        for (int i = 0; i < num_bins_ && i < MAX_BINS; ++i) {
            total = (total > UINT32_MAX - store_[i]) ? UINT32_MAX
                                                     : total + store_[i];
        }
        std::fill(store_.begin(), store_.end(), 0u);
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
        std::uint32_t collapsed = 0;
        for (int i = 0; i < collapse_count_bins && i < MAX_BINS; ++i) {
            collapsed = (collapsed > UINT32_MAX - store_[i])
                            ? UINT32_MAX
                            : collapsed + store_[i];
            store_[i] = 0;
        }

        int remaining = num_bins_ - collapse_count_bins;
        if (remaining > 0 && collapse_count_bins > 0) {
            std::memmove(
                &store_[0], &store_[collapse_count_bins],
                static_cast<std::size_t>(std::min(remaining, MAX_BINS)) *
                    sizeof(std::uint32_t));
            int clear_start = std::min(remaining, MAX_BINS);
            int clear_count =
                std::min(collapse_count_bins, MAX_BINS - clear_start);
            if (clear_count > 0) {
                std::memset(&store_[clear_start], 0,
                            static_cast<std::size_t>(clear_count) *
                                sizeof(std::uint32_t));
            }
        }

        store_[0] = (store_[0] > UINT32_MAX - collapsed)
                        ? UINT32_MAX
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
                    static_cast<std::size_t>(to_move) * sizeof(std::uint32_t));
            }
            std::memset(
                &store_[0], 0,
                static_cast<std::size_t>(shift) * sizeof(std::uint32_t));
        }
        offset_ = new_min_key;
        min_key_ = new_min_key;
    }

    max_key_ = new_max_key;
    num_bins_ = std::min(max_key_ - min_key_ + 1, MAX_BINS);
    collapsed_ = true;
}

template <int BINS>
std::vector<HistogramBin> BasicDDSketch<BINS>::bins() const {
    std::vector<HistogramBin> out;
    if (zero_count_ > 0)
        out.push_back({0.0, 0.0, static_cast<std::uint64_t>(zero_count_)});
    if (initialized_) {
        for (int i = 0; i < num_bins_ && i < MAX_BINS; ++i) {
            if (store_[i] == 0) continue;
            const int idx = i + offset_;
            out.push_back({bin_lower_bound(idx), bin_upper_bound(idx),
                           static_cast<std::uint64_t>(store_[i])});
        }
    }
    return out;
}

template <int BINS>
void BasicDDSketch<BINS>::add(double value, double weight) {
    if (weight <= 0.0) return;

    auto w = static_cast<std::uint32_t>(
        std::min(weight, static_cast<double>(UINT32_MAX)));
    if (w == 0) w = 1;

    if (value < min_) min_ = value;
    if (value > max_) max_ = value;

    count_ += w;

    if (value == 0.0) {
        zero_count_ += w;
        return;
    }

    double abs_value = std::abs(value);
    int idx = static_cast<int>(std::ceil(std::log(abs_value) / log_gamma_));
    add_to_bin(idx, w);
}

template <int BINS>
void BasicDDSketch<BINS>::add_key(std::int32_t key, double value,
                                  std::uint32_t weight) {
    if (weight == 0) weight = 1;
    if (value < min_) min_ = value;
    if (value > max_) max_ = value;
    count_ += weight;
    if (key == SKETCH_ZERO_KEY) {
        zero_count_ += weight;
        return;
    }
    add_to_bin(static_cast<int>(key), weight);
}

template <int BINS>
void BasicDDSketch<BINS>::reserve_keys(int min_key, int max_key) {
    if constexpr (GROWS) {
        if (max_key - min_key + 1 > MAX_BINS) min_key = max_key - MAX_BINS + 1;
        if (!initialized_) {
            const int span = max_key - min_key + 1;
            grow_to(std::min(span + RESERVE_MARGIN, MAX_BINS));
            offset_ = min_key;
            min_key_ = min_key;
            max_key_ = min_key;
            num_bins_ = 1;
            initialized_ = true;
            return;
        }
        const int lo = std::min(min_key, min_key_);
        const int hi = std::max(max_key, max_key_);
        if (hi - lo + 1 > MAX_BINS) return;
        const int span = hi - lo + 1;
        if (store_.size() < static_cast<std::size_t>(span))
            grow_to(std::min(span + RESERVE_MARGIN, MAX_BINS));
        if (lo < min_key_) {
            const int shift = min_key_ - lo;
            std::memmove(
                &store_[shift], &store_[0],
                static_cast<std::size_t>(num_bins_) * sizeof(std::uint32_t));
            std::memset(
                &store_[0], 0,
                static_cast<std::size_t>(shift) * sizeof(std::uint32_t));
            offset_ = lo;
            min_key_ = lo;
            num_bins_ = max_key_ - min_key_ + 1;
        }
    } else {
        (void)min_key;
        (void)max_key;
    }
}

template <int BINS>
void BasicDDSketch<BINS>::merge(const BasicDDSketch& other) {
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
    grow_to(std::min(merged_max - merged_min + 1, MAX_BINS));

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
                    static_cast<std::size_t>(to_move) * sizeof(std::uint32_t));
            }
            std::memset(
                &store_[0], 0,
                static_cast<std::size_t>(shift) * sizeof(std::uint32_t));
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
        std::uint32_t v = other.store_[other_pos];
        if (v == 0) continue;

        int my_pos = k - offset_;
        if (my_pos < 0) {
            store_[0] =
                (store_[0] > UINT32_MAX - v) ? UINT32_MAX : store_[0] + v;
        } else if (my_pos < MAX_BINS) {
            store_[my_pos] = (store_[my_pos] > UINT32_MAX - v)
                                 ? UINT32_MAX
                                 : store_[my_pos] + v;
        }
    }
}

template <int BINS>
double BasicDDSketch<BINS>::quantile(double q) const {
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

template <int BINS>
void BasicDDSketch<BINS>::reset() {
    store_ = {};
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

template <int BINS>
std::size_t BasicDDSketch<BINS>::memory_usage() const {
    if constexpr (GROWS)
        return sizeof(BasicDDSketch) +
               store_.capacity() * sizeof(std::uint32_t);
    return sizeof(BasicDDSketch);
}

namespace {
constexpr std::size_t SERIALIZED_HEADER_SIZE =
    sizeof(double) * 3 + sizeof(std::uint64_t) * 2 + sizeof(std::int32_t) +
    sizeof(std::uint32_t);
}  // namespace

template <int BINS>
std::size_t BasicDDSketch<BINS>::serialized_size() const {
    const auto num = static_cast<std::size_t>(
        std::min(num_bins_, static_cast<int>(MAX_BINS)));
    return SERIALIZED_HEADER_SIZE + num * sizeof(std::uint32_t);
}

template <int BINS>
std::vector<std::uint8_t> BasicDDSketch<BINS>::serialize() const {
    std::vector<std::uint8_t> buf;
    serialize_into(buf);
    return buf;
}

template <int BINS>
void BasicDDSketch<BINS>::write_to(std::uint8_t* p) const {
    const auto num = static_cast<std::uint32_t>(
        std::min(num_bins_, static_cast<int>(MAX_BINS)));

    write_f64_le(p, gamma_);
    write_f64_le(p + 8, min_);
    write_f64_le(p + 16, max_);
    write_u64_le(p + 24, count_);
    write_u64_le(p + 32, zero_count_);
    write_u32_le(p + 40, static_cast<std::uint32_t>(offset_));
    write_u32_le(p + 44, num);
    p += SERIALIZED_HEADER_SIZE;
    for (std::uint32_t i = 0; i < num; ++i)
        write_u32_le(p + std::size_t{i} * 4, store_[i]);
}

template <int BINS>
BasicDDSketch<BINS> BasicDDSketch<BINS>::deserialize(const std::uint8_t* data,
                                                     std::size_t len) {
    LeReader r(data, len);
    BasicDDSketch s;
    std::int32_t off32 = 0;
    std::uint32_t num = 0;
    if (!r.f64(s.gamma_)) return BasicDDSketch{};
    if (!std::isfinite(s.gamma_) || s.gamma_ <= 1.0) return BasicDDSketch{};
    s.log_gamma_ = std::log(s.gamma_);
    if (!r.f64(s.min_) || !r.f64(s.max_) || !r.u64(s.count_) ||
        !r.u64(s.zero_count_) || !r.i32(off32) || !r.u32(num))
        return BasicDDSketch{};
    // Keys of a real value stay far inside this range; a wider offset would
    // overflow the key arithmetic in add and merge.
    constexpr std::int32_t MAX_KEY_MAGNITUDE = 1 << 29;
    if (off32 < -MAX_KEY_MAGNITUDE || off32 > MAX_KEY_MAGNITUDE)
        return BasicDDSketch{};
    s.offset_ = static_cast<int>(off32);

    if (num > MAX_BINS) num = MAX_BINS;
    const std::uint8_t* bins = r.take(std::size_t{num} * sizeof(std::uint32_t));
    if (!bins) return BasicDDSketch{};

    s.ensure_bins(static_cast<int>(num));
    for (std::uint32_t i = 0; i < num; ++i)
        s.store_[i] = read_u32_le(bins + std::size_t{i} * 4);
    s.min_key_ = s.offset_;
    s.max_key_ = s.offset_ + static_cast<int>(num) - 1;
    s.num_bins_ = static_cast<int>(num);
    s.initialized_ = s.count_ > 0;

    return s;
}

template class BasicDDSketch<128>;
template class BasicDDSketch<2048>;

}  // namespace dftracer::utils::dataframe
