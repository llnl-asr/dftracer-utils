#ifndef DFTRACER_UTILS_UTILITIES_TEXT_SHARED_H
#define DFTRACER_UTILS_UTILITIES_TEXT_SHARED_H

#include <dftracer/utils/utilities/hash/hash.h>
#include <dftracer/utils/utilities/io/lines/line_types.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::utilities::text {

/**
 * @brief Represents raw text content.
 */
struct Text {
    std::string content;

    Text() = default;
    explicit Text(std::string str) : content(std::move(str)) {}
    explicit Text(const char* str) : content(str) {}

    bool empty() const { return content.empty(); }
    std::size_t size() const { return content.size(); }
};

/**
 * @brief A line with a predicate function for filtering.
 */
struct FilterableLine {
    io::lines::Line line;
    std::function<bool(const io::lines::Line&)> predicate;

    FilterableLine() = default;

    FilterableLine(io::lines::Line l,
                   std::function<bool(const io::lines::Line&)> pred)
        : line(std::move(l)), predicate(std::move(pred)) {}

    // Note: Cannot easily define equality for std::function, so filtering
    // utilities should not use Cacheable tag unless they provide a custom key
};

}  // namespace dftracer::utils::utilities::text

// Hash specializations to enable caching
namespace std {
template <>
struct hash<dftracer::utils::utilities::hash::Hash> {
    std::size_t operator()(
        const dftracer::utils::utilities::hash::Hash& hash_val) const noexcept {
        return std::hash<std::size_t>{}(hash_val.value);
    }
};
}  // namespace std

#endif  // DFTRACER_UTILS_UTILITIES_TEXT_SHARED_H
