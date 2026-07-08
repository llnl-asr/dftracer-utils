#include <dftracer/utils/utilities/common/statistics/log2_histogram.h>
#include <simdjson.h>

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace dftracer::utils::utilities::common::statistics {

std::size_t Log2Histogram::bin_index(std::uint64_t value) {
    if (value == 0) return 0;
    // floor(log2(value)) + 1
    // __builtin_clzll: count leading zeros for unsigned long long (64-bit)
    return static_cast<std::size_t>(63 - __builtin_clzll(value)) + 1;
}

std::uint64_t Log2Histogram::bin_lower(std::size_t bin) {
    if (bin == 0) return 0;
    if (bin == 1) return 1;
    return static_cast<std::uint64_t>(1) << (bin - 1);
}

std::uint64_t Log2Histogram::bin_upper(std::size_t bin) {
    if (bin == 0) return 0;
    if (bin >= 64) return std::numeric_limits<std::uint64_t>::max();
    return (static_cast<std::uint64_t>(1) << bin);
}

void Log2Histogram::add(std::uint64_t value, std::uint64_t count) {
    std::size_t idx = bin_index(value);
    bins_[idx] += count;
    total_count_ += count;
}

void Log2Histogram::merge(const Log2Histogram& other) {
    for (std::size_t i = 0; i < NUM_BINS; ++i) {
        bins_[i] += other.bins_[i];
    }
    total_count_ += other.total_count_;
}

double Log2Histogram::approx_percentile(double p) const {
    if (total_count_ == 0) return 0.0;
    if (p <= 0.0) return 0.0;
    if (p >= 1.0) {
        // Find the highest non-empty bin
        for (std::size_t i = NUM_BINS; i > 0; --i) {
            if (bins_[i - 1] > 0) {
                return static_cast<double>(bin_upper(i - 1));
            }
        }
        return 0.0;
    }

    double target = p * static_cast<double>(total_count_);
    double cumulative = 0.0;

    for (std::size_t i = 0; i < NUM_BINS; ++i) {
        if (bins_[i] == 0) continue;
        cumulative += static_cast<double>(bins_[i]);
        if (cumulative >= target) {
            // Interpolate within the bin
            double prev_cumulative = cumulative - static_cast<double>(bins_[i]);
            double fraction =
                (target - prev_cumulative) / static_cast<double>(bins_[i]);
            double lo = static_cast<double>(bin_lower(i));
            double hi = static_cast<double>(bin_upper(i));
            return lo + fraction * (hi - lo);
        }
    }

    return 0.0;
}

namespace {
std::string format_count(std::uint64_t count) {
    if (count < 1000) return std::to_string(count);
    std::string s = std::to_string(count);
    std::string result;
    int pos = 0;
    int len = static_cast<int>(s.size());
    for (int i = 0; i < len; ++i) {
        if (pos > 0 && (len - i) % 3 == 0) result += ',';
        result += s[i];
        ++pos;
    }
    return result;
}

std::string format_range(std::uint64_t lo, std::uint64_t hi,
                         const std::string& unit) {
    char buf[128];
    if (lo == hi && lo == 0) {
        std::snprintf(buf, sizeof(buf), "0 %s", unit.c_str());
    } else {
        std::snprintf(buf, sizeof(buf), "[%s, %s) %s", format_count(lo).c_str(),
                      format_count(hi).c_str(), unit.c_str());
    }
    return buf;
}
}  // namespace

std::string Log2Histogram::render_ascii(std::size_t max_width,
                                        const std::string& unit) const {
    if (total_count_ == 0) return "    (no data)\n";

    // Find max count for scaling
    std::uint64_t max_count = 0;
    for (std::size_t i = 0; i < NUM_BINS; ++i) {
        if (bins_[i] > max_count) max_count = bins_[i];
    }

    std::ostringstream out;
    for (std::size_t i = 0; i < NUM_BINS; ++i) {
        if (bins_[i] == 0) continue;

        std::string range = format_range(bin_lower(i), bin_upper(i), unit);
        std::string count_str = format_count(bins_[i]);

        // Scale bar
        std::size_t bar_len = 0;
        if (max_count > 0) {
            bar_len = static_cast<std::size_t>(static_cast<double>(bins_[i]) /
                                               static_cast<double>(max_count) *
                                               static_cast<double>(max_width));
            if (bar_len == 0 && bins_[i] > 0) bar_len = 1;
        }

        char line[256];
        std::snprintf(line, sizeof(line), "    %-24s |%-*s %s\n", range.c_str(),
                      static_cast<int>(max_width),
                      std::string(bar_len, '#').c_str(), count_str.c_str());
        out << line;
    }

    return out.str();
}

std::string Log2Histogram::render_blocks(std::size_t max_width,
                                         const std::string& unit,
                                         const std::string& indent) const {
    if (total_count_ == 0) return indent + "(no data)\n";

    // Unicode block elements: 8 levels from thinnest to fullest
    static const char* blocks[] = {
        " ",      "\u2581",  // ▁
        "\u2582",            // ▂
        "\u2583",            // ▃
        "\u2584",            // ▄
        "\u2585",            // ▅
        "\u2586",            // ▆
        "\u2587",            // ▇
        "\u2588"             // █
    };

    std::uint64_t max_count = 0;
    for (std::size_t i = 0; i < NUM_BINS; ++i) {
        if (bins_[i] > max_count) max_count = bins_[i];
    }

    std::ostringstream out;
    for (std::size_t i = 0; i < NUM_BINS; ++i) {
        if (bins_[i] == 0) continue;

        std::string range = format_range(bin_lower(i), bin_upper(i), unit);
        std::string count_str = format_count(bins_[i]);

        // Scale to max_width full blocks + fractional last block
        double scaled = static_cast<double>(bins_[i]) /
                        static_cast<double>(max_count) *
                        static_cast<double>(max_width);
        auto full_blocks = static_cast<std::size_t>(scaled);
        int frac =
            static_cast<int>((scaled - static_cast<double>(full_blocks)) * 8.0);
        if (full_blocks == 0 && frac == 0 && bins_[i] > 0) frac = 1;

        std::string bar;
        for (std::size_t b = 0; b < full_blocks; ++b) {
            bar += blocks[8];
        }
        if (frac > 0 && full_blocks < max_width) {
            bar += blocks[frac];
        }

        // Pad to fixed display width
        std::size_t display_len = full_blocks + (frac > 0 ? 1 : 0);
        std::string padding;
        if (display_len < max_width) {
            padding = std::string(max_width - display_len, ' ');
        }

        char label[128];
        std::snprintf(label, sizeof(label), "%-24s", range.c_str());
        out << indent << label << bar << padding << " " << count_str << "\n";
    }

    return out.str();
}

std::string Log2Histogram::to_json() const {
    std::ostringstream ss;
    ss << '[';
    bool first = true;
    for (std::size_t i = 0; i < NUM_BINS; ++i) {
        if (bins_[i] == 0) continue;
        if (!first) ss << ',';
        first = false;
        ss << '[' << i << ',' << bins_[i] << ']';
    }
    ss << ']';
    return ss.str();
}

std::string Log2Histogram::to_json_detailed() const {
    std::ostringstream ss;
    ss << '[';
    bool first = true;
    for (std::size_t i = 0; i < NUM_BINS; ++i) {
        if (bins_[i] == 0) continue;
        if (!first) ss << ',';
        first = false;
        ss << "{\"bin\":" << i << ",\"lo\":" << bin_lower(i) << ",\"hi\":";
        // Top bin is unbounded (bin_upper is UINT64_MAX); emit null instead.
        if (i >= 64) {
            ss << "null";
        } else {
            ss << bin_upper(i);
        }
        ss << ",\"count\":" << bins_[i] << '}';
    }
    ss << ']';
    return ss.str();
}

Log2Histogram Log2Histogram::from_json(const std::string& json) {
    Log2Histogram hist;

    simdjson::dom::parser parser;
    auto result = parser.parse(json.data(), json.size());
    if (result.error()) return hist;

    auto root = result.value_unsafe();
    if (!root.is_array()) return hist;

    simdjson::dom::array root_arr;
    if (root.get(root_arr)) return hist;

    for (auto pair : root_arr) {
        if (!pair.is_array()) continue;
        simdjson::dom::array arr;
        if (pair.get(arr)) continue;
        if (arr.size() != 2) continue;

        auto bin_idx_result = arr.at(0).get_uint64();
        auto count_result = arr.at(1).get_uint64();
        if (bin_idx_result.error() || count_result.error()) continue;

        std::size_t bin_idx =
            static_cast<std::size_t>(bin_idx_result.value_unsafe());
        std::uint64_t count = count_result.value_unsafe();
        if (bin_idx < NUM_BINS) {
            hist.bins_[bin_idx] += count;
            hist.total_count_ += count;
        }
    }
    return hist;
}

}  // namespace dftracer::utils::utilities::common::statistics
