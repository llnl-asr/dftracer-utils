// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_ARROW_ARROW_H
#define DFTRACER_UTILS_ARROW_ARROW_H

#include <dftracer/utils/arrow/batch.h>
#include <dftracer/utils/arrow/c_api.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/result.h>

namespace dftracer::utils::arrow {

/**
 * @brief Exports `col` as one @ref Batch, zero copy: the array's buffers alias
 * the column's, and the array's release keeps them alive.
 *
 * A FLAT column exports as a primitive array and a SELECTION column as a
 * dictionary array (indices = the selection, dictionary = the base). An
 * InvalidArgument error for a column that holds no buffers.
 */
dstd::Result<Batch> to_arrow(const dataframe::Series& col);

/**
 * @brief Exports `frame` as one @ref dftracer::utils::arrow::Batch "Batch": a
 * struct array with one child per column, zero copy, named by the frame's
 * column names.
 *
 * An InvalidArgument error for a frame with a column that holds no buffers.
 */
dstd::Result<Batch> to_arrow(const dataframe::DataFrame& frame);

/**
 * @brief Imports `batch` as a Series, wrapping its buffers zero copy.
 *
 * Takes ownership of everything `batch` holds, so the Series keeps the buffers
 * alive. An InvalidArgument error for a batch with no buffers, or for an Arrow
 * type the engine cannot hold.
 */
dstd::Result<dataframe::Series> series_from_arrow(Batch&& batch);

/**
 * @brief Imports a struct @p batch as a DataFrame: one column per struct child,
 * named by its child schema, zero copy, taking ownership of everything the
 * batch holds.
 *
 * An InvalidArgument error for a batch that is not a struct, and a NotFound
 * error naming the first child whose type the engine cannot hold.
 */
dstd::Result<dataframe::DataFrame> dataframe_from_arrow(Batch&& batch);

}  // namespace dftracer::utils::arrow

#endif  // DFTRACER_UTILS_ARROW_ARROW_H
