// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/timestamp_histogram.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/varint.h>
#include <dftracer/utils/dstd/vector.h>

#include <algorithm>
#include <cstring>

namespace dftracer::utils::dataframe {

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

    dstd::Vector<dstd::Pair<std::uint64_t, std::uint64_t>> merged;
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

dstd::Vector<double> TimestampHistogram::expansion_weights(
    std::uint64_t bucket_start_us, std::uint64_t bucket_end_us,
    std::size_t sub_bucket_count) const {
    dstd::Vector<double> weights(sub_bucket_count, 0.0);
    if (sub_bucket_count == 0 || bucket_start_us >= bucket_end_us)
        return weights;

    std::uint64_t sub_width =
        (bucket_end_us - bucket_start_us) / sub_bucket_count;
    if (sub_width == 0) sub_width = 1;

    std::uint64_t total_in_range = 0;
    for (std::size_t i = 0; i < sub_bucket_count; ++i) {
        std::uint64_t sub_start = bucket_start_us + i * sub_width;
        std::uint64_t sub_end = (i + 1 < sub_bucket_count)
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
        double uniform = 1.0 / static_cast<double>(sub_bucket_count);
        for (auto& w : weights) w = uniform;
    }

    return weights;
}

dstd::Vector<std::uint8_t> TimestampHistogram::serialize() const {
    dstd::Vector<std::uint8_t> out;
    out.reserve(bins_.size() * 6 + 16);

    dstd::append_varint(out, total_count_);
    dstd::append_varint(out, bins_.size());

    std::uint64_t prev_idx = 0;
    for (const auto& [idx, count] : bins_) {
        dstd::append_varint(out, idx - prev_idx);
        dstd::append_varint(out, count);
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

    std::uint64_t bin_count = 0;
    if (!dstd::read_varint(ptr, end, hist.total_count_)) return hist;
    if (!dstd::read_varint(ptr, end, bin_count)) return hist;

    // A bin takes at least two bytes, so the blob bounds a corrupt count.
    hist.bins_.reserve(std::min<std::uint64_t>(
        bin_count, static_cast<std::uint64_t>(end - ptr) / 2));
    std::uint64_t prev_idx = 0;
    for (std::uint64_t i = 0; i < bin_count; ++i) {
        std::uint64_t delta = 0;
        std::uint64_t count = 0;
        if (!dstd::read_varint(ptr, end, delta) ||
            !dstd::read_varint(ptr, end, count)) {
            break;
        }
        prev_idx += delta;
        hist.bins_.push_back({prev_idx, count});
    }

    return hist;
}

}  // namespace dftracer::utils::dataframe
