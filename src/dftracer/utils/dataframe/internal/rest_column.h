#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_REST_COLUMN_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_REST_COLUMN_H

#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/series.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dftracer::utils::dataframe {

/// The plan column that carries, as one Struct, the columns a scan returns
/// beyond the plan's schema. Never part of a collected frame or a public
/// schema.
inline constexpr std::string_view REST_COLUMN = "__rest";

/// A flat Struct of `rows` rows over `fields` named `names`. Unlike
/// Series::structs it keeps the length of a struct with no fields.
inline Series struct_of_length(std::vector<std::string> names,
                               std::vector<Series> fields, std::int64_t rows) {
    auto* col = new dftu_series();
    col->type = TypeId::Struct;
    col->encoding = Encoding::Flat;
    col->length = rows;
    col->nested.reserve(fields.size());
    for (std::size_t i = 0; i < fields.size(); ++i)
        col->nested.push_back(
            {std::shared_ptr<dftu_series>(fields[i].release()),
             std::move(names[i])});
    return Series{col};
}

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_REST_COLUMN_H
