#ifndef DFTRACER_UTILS_INDEX_STORE_COLUMN_FAMILIES_H
#define DFTRACER_UTILS_INDEX_STORE_COLUMN_FAMILIES_H

#include <array>
#include <string_view>

namespace dftracer::utils::index::store::cf {

inline constexpr std::string_view DEFAULT = "default";
inline constexpr std::string_view MEMBERS = "members";
inline constexpr std::string_view GRANULE = "granule";
inline constexpr std::string_view BLOB = "blob";
inline constexpr std::string_view POSTINGS = "postings";
inline constexpr std::string_view AGGREGATION = "aggregation";
inline constexpr std::string_view SYSTEM_METRICS = "system_metrics";
inline constexpr std::string_view ROLLUP = "rollup";
inline constexpr auto ALL =
    std::to_array<std::string_view>({DEFAULT, MEMBERS, GRANULE, BLOB, POSTINGS,
                                     AGGREGATION, SYSTEM_METRICS, ROLLUP});

}  // namespace dftracer::utils::index::store::cf

#endif  // DFTRACER_UTILS_INDEX_STORE_COLUMN_FAMILIES_H
