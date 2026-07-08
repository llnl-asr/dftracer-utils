#ifndef DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_LOG2_HISTOGRAM_H
#define DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_LOG2_HISTOGRAM_H

#include <array>
#include <cstdint>
#include <string>

namespace dftracer::utils::utilities::common::statistics {

/**
 * @brief Log2-scale histogram with 65 fixed bins.
 *
 * Bin 0: value == 0
 * Bin k (1 <= k <= 64): values in [2^(k-1), 2^k)
 *
 * Covers the full uint64_t range (0 to 2^63). Suitable for both
 * microsecond durations and byte sizes.
 */
class Log2Histogram {
   public:
    static constexpr std::size_t NUM_BINS = 65;

    Log2Histogram() = default;

    void add(std::uint64_t value, std::uint64_t count = 1);
    void merge(const Log2Histogram& other);
    double approx_percentile(double p) const;
    std::string render_ascii(std::size_t max_width,
                             const std::string& unit) const;
    std::string render_blocks(std::size_t max_width, const std::string& unit,
                              const std::string& indent = "      ") const;
    std::string to_json() const;
    std::string to_json_detailed() const;
    static Log2Histogram from_json(const std::string& json);

    std::uint64_t total_count() const { return total_count_; }
    const std::array<std::uint64_t, NUM_BINS>& bins() const { return bins_; }

    static std::size_t bin_index(std::uint64_t value);
    static std::uint64_t bin_lower(std::size_t bin);
    static std::uint64_t bin_upper(std::size_t bin);

   private:
    std::array<std::uint64_t, NUM_BINS> bins_ = {};
    std::uint64_t total_count_ = 0;
};

}  // namespace dftracer::utils::utilities::common::statistics

#endif  // DFTRACER_UTILS_UTILITIES_COMMON_STATISTICS_LOG2_HISTOGRAM_H
