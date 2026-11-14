// SPDX-License-Identifier: MIT
#include <dftracer/utils/arrow/arrow.h>
#include <dftracer/utils/arrow/c_api.h>
#include <dftracer/utils/arrow/internal.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <utility>

namespace dftracer::utils::arrow {

using dataframe::DataFrame;
using dataframe::Series;

namespace {

Series as_struct(const DataFrame& frame, bool materialize) {
    dstd::Vector<dstd::String> field_names = dstd::to_strings(frame.names);
    dstd::Vector<Series> field_columns;
    field_columns.reserve(frame.columns.size());
    for (const Series& col : frame.columns)
        field_columns.push_back(
            materialize && !col.is_flat() ? col.materialize() : col.share());
    return Series::structs(std::move(field_names), std::move(field_columns));
}

}  // namespace

dstd::Result<Batch> to_arrow(const Series& col) {
    if (!col.valid())
        return common::make_error<"to_arrow: a column with no buffers">(
            common::ErrorCode::InvalidArgument);
    ArrowSchema schema;
    ArrowArray array;
    detail::to_arrow(col, &schema, &array);
    return Batch::adopt(&schema, &array);
}

dstd::Result<Batch> to_arrow(const DataFrame& frame) {
    for (std::size_t i = 0; i < frame.columns.size(); ++i) {
        if (frame.columns[i].valid()) continue;
        return common::make_error<"to_arrow: column '{}' has no buffers">(
            common::ErrorCode::InvalidArgument, frame.names[i]);
    }
    Series st = as_struct(frame, false);
    ArrowSchema schema;
    ArrowArray array;
    detail::to_arrow(st, &schema, &array);
    return Batch::adopt(&schema, &array);
}

dstd::Result<Series> series_from_arrow(Batch&& batch) {
    if (!batch.is_valid())
        return common::make_error<"series_from_arrow: a batch with no data">(
            common::ErrorCode::InvalidArgument);
    ArrowSchema schema;
    ArrowArray array;
    batch.release_into(&schema, &array);
    Series col = detail::import_series(&schema, &array, nullptr);
    if (schema.release != nullptr) schema.release(&schema);
    if (!col.valid())
        return common::make_error<
            "series_from_arrow: an Arrow type the engine cannot hold">(
            common::ErrorCode::InvalidArgument);
    return col;
}

dstd::Result<DataFrame> dataframe_from_arrow(Batch&& batch) {
    if (!batch.is_valid())
        return common::make_error<"dataframe_from_arrow: a batch with no data">(
            common::ErrorCode::InvalidArgument);
    ArrowSchema schema;
    ArrowArray array;
    batch.release_into(&schema, &array);
    // `import_frame` takes the array only when it is a struct, so the shape is
    // checked here and both structs are released on that path.
    if (!detail::is_struct(&schema)) {
        if (schema.release != nullptr) schema.release(&schema);
        if (array.release != nullptr) array.release(&array);
        return common::make_error<"dataframe_from_arrow: not a struct array">(
            common::ErrorCode::InvalidArgument);
    }
    DataFrame df = detail::import_frame(&schema, &array, nullptr);
    if (schema.release != nullptr) schema.release(&schema);
    for (std::size_t i = 0; i < df.columns.size(); ++i) {
        if (df.columns[i].valid()) continue;
        return common::make_error<
            "dataframe_from_arrow: an Arrow type the engine cannot hold in "
            "column '{}'">(common::ErrorCode::NotFound, df.names[i]);
    }
    return df;
}

}  // namespace dftracer::utils::arrow
