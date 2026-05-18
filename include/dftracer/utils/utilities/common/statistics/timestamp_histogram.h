#ifndef DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_TIMESTAMP_HISTOGRAM_H
#define DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_TIMESTAMP_HISTOGRAM_H

#include <cstdint>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::common::statistics {

class TimestampHistogram {
   public:
    static constexpr std::uint64_t BIN_WIDTH_US = 100'000;  // 100ms

    TimestampHistogram() = default;

    void add(std::uint64_t timestamp_us);
    void merge(const TimestampHistogram& other);

    std::uint64_t count_in_range(std::uint64_t ts_start_us,
                                 std::uint64_t ts_end_us) const;
    double selectivity(std::uint64_t ts_start_us,
                       std::uint64_t ts_end_us) const;
    std::vector<double> expansion_weights(std::uint64_t bucket_start_us,
                                          std::uint64_t bucket_end_us,
                                          std::size_t num_sub_buckets) const;

    std::vector<std::uint8_t> serialize() const;
    static TimestampHistogram deserialize(const std::uint8_t* data,
                                          std::size_t len);

    std::uint64_t total_count() const { return total_count_; }
    bool empty() const { return bins_.empty(); }
    std::size_t num_bins() const { return bins_.size(); }

    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& bins() const {
        return bins_;
    }

    static std::uint64_t bin_index(std::uint64_t timestamp_us) {
        return timestamp_us / BIN_WIDTH_US;
    }

    static std::uint64_t bin_start_us(std::uint64_t bin_idx) {
        return bin_idx * BIN_WIDTH_US;
    }

    static std::uint64_t bin_end_us(std::uint64_t bin_idx) {
        return (bin_idx + 1) * BIN_WIDTH_US;
    }

   private:
    // Sorted by bin_index. Sparse: only non-zero bins stored.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> bins_;
    std::uint64_t total_count_ = 0;
};

}  // namespace dftracer::utils::utilities::common::statistics

#endif  // DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_TIMESTAMP_HISTOGRAM_H
