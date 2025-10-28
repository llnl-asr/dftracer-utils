#ifndef DFTRACER_UTILS_UTILITIES_IO_TYPES_RAW_DATA_H
#define DFTRACER_UTILS_UTILITIES_IO_TYPES_RAW_DATA_H

#include <cstring>
#include <initializer_list>
#include <string>
#include <type_traits>
#include <vector>

namespace dftracer::utils::utilities::io {

/**
 * @brief Generic binary/raw data structure.
 *
 * This is a fundamental I/O type that represents raw bytes.
 * Used by:
 * - BinaryFileReader output
 * - Compression input/output
 * - Binary data manipulation utilities
 */
struct RawData {
    std::vector<unsigned char> data;

    RawData() = default;

    explicit RawData(std::vector<unsigned char> d) : data(std::move(d)) {}

    // Constructor from initializer list (handles multiple values)
    // Usage: RawData data{0x01, 0x02, 0x03};
    RawData(std::initializer_list<unsigned char> ilist) : data(ilist) {}

    // String constructor
    RawData(const std::string& str) : data(str.begin(), str.end()) {}

    // C-string constructor
    RawData(const char* str) : data(str, str + std::strlen(str)) {}

    // Variadic template constructor using SFINAE to avoid ambiguity
    // Only enabled for multiple arguments that are not strings
    // Usage: RawData data(0x01, 0x02, 0x03); or RawData data('A', 'B', 'C');
    template <
        typename First, typename... Rest,
        typename = std::enable_if_t<
            (sizeof...(Rest) > 0) &&  // At least 2 arguments
            !std::is_convertible_v<First, std::string> &&  // Not a string
            !std::is_convertible_v<First, const char*>>>   // Not a C-string
    explicit RawData(First first, Rest... rest) {
        data = {static_cast<unsigned char>(first),
                static_cast<unsigned char>(rest)...};
    }

    // Equality for caching support
    bool operator==(const RawData& other) const { return data == other.data; }

    bool operator!=(const RawData& other) const { return !(*this == other); }

    // Convert to string (useful for text data)
    std::string to_string() const {
        return std::string(data.begin(), data.end());
    }

    std::size_t size() const { return data.size(); }

    bool empty() const { return data.empty(); }
};

}  // namespace dftracer::utils::utilities::io

// Hash specialization for caching
namespace std {
template <>
struct hash<dftracer::utils::utilities::io::RawData> {
    std::size_t operator()(
        const dftracer::utils::utilities::io::RawData& raw) const noexcept {
        // Hash the data bytes
        std::size_t h = 0;
        for (const auto& byte : raw.data) {
            h ^= std::hash<unsigned char>{}(byte) + 0x9e3779b9 + (h << 6) +
                 (h >> 2);
        }
        return h;
    }
};
}  // namespace std

#endif  // DFTRACER_UTILS_UTILITIES_IO_TYPES_RAW_DATA_H
