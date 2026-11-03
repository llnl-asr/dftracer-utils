// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_TEXT_SHARED_H
#define DFTRACER_UTILS_UTILITIES_TEXT_SHARED_H

#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/hasher.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/utilities/fileio/lines/line_types.h>
#include <dftracer/utils/utilities/hash/hash.h>

#include <functional>
#include <string>

namespace dftracer::utils::utilities::text {

/**
 * @brief Represents raw text content.
 */
struct Text {
    dstd::String content;

    Text() = default;
    explicit Text(dstd::String str) : content(std::move(str)) {}
    explicit Text(const char* str) : content(str) {}

    bool empty() const { return content.empty(); }
    std::size_t size() const { return content.size(); }
};

/**
 * @brief A line with a predicate function for filtering.
 */
struct FilterableLine {
    fileio::lines::Line line;
    dstd::Function<bool(const fileio::lines::Line&)> predicate;

    FilterableLine() = default;

    FilterableLine(fileio::lines::Line l,
                   dstd::Function<bool(const fileio::lines::Line&)> pred)
        : line(std::move(l)), predicate(std::move(pred)) {}

    // Note: Cannot easily define equality for std::function, so filtering
    // utilities should not use Cacheable tag unless they provide a custom key
};

}  // namespace dftracer::utils::utilities::text

// Hash specializations to enable caching
namespace dftracer::utils::dstd {
template <>
struct Hash<dftracer::utils::utilities::hash::Hash> {
    using is_avalanching = void;

    std::size_t operator()(
        const dftracer::utils::utilities::hash::Hash& hash_val) const noexcept {
        return Hash<std::size_t>{}(hash_val.value);
    }
};
}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_UTILITIES_TEXT_SHARED_H
