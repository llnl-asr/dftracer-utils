// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_ARROW_STREAM_H
#define DFTRACER_UTILS_ARROW_STREAM_H

#include <dftracer/utils/arrow/c_api.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/runtime/runtime.h>

namespace dftracer::utils::arrow {

/**
 * @brief Fills `out` with a stream that yields `col` as Arrow arrays: one
 * array per chunk of a CHUNKED column, else one array.
 *
 * The schema is the column's own type, not a struct, so a consumer reads the
 * stream as one (Chunked)Array. Each array is converted inside `get_next`, so
 * opening the stream converts nothing.
 *
 * On success `out` owns everything and stays valid until its `release` runs,
 * which it does exactly once. A consumer must call `get_schema` before
 * `get_next`, and `get_next` returns success with `release == nullptr` at the
 * end, as the Arrow C Stream Interface says. A failure returns EINVAL, EIO or
 * ENOMEM to `get_schema`/`get_next`, never throws, and reports the message
 * through `get_last_error`; every later `get_next` fails the same way.
 *
 * An InvalidArgument error for a null `out` or a column that holds no
 * buffers.
 */
dstd::Result<void> to_arrow_stream(const dataframe::Series& col,
                                   ArrowArrayStream* out);

/**
 * @brief Fills `out` with a stream that yields `frame` as struct arrays: one
 * batch per chunk when every column is chunked on the same boundaries, else
 * row slices of @ref
 * dftracer::utils::common::constants::dataframe::BATCH_SIZE rows that share
 * the frame's buffers.
 *
 * Each batch is converted inside `get_next`; the schema names one child per
 * column. The errors, the end of stream and the failure behavior are those of
 * @ref to_arrow_stream(const dataframe::Series&, ArrowArrayStream*).
 */
dstd::Result<void> to_arrow_stream(const dataframe::DataFrame& frame,
                                   ArrowArrayStream* out);

/**
 * @brief Fills `out` with a stream over `plan`: the stream owns the query, and
 * `runtime` runs the background task that drains
 * @ref dataframe::LazyFrame::stream into a bounded channel.
 *
 * `get_next` blocks until the next batch arrives, converts it and returns it.
 * `release` cancels the query and waits for that task to end, so it returns
 * only once the query stopped. Both are blocking calls: never call
 * `get_schema`, `get_next` or `release` from a worker of `runtime`, which would
 * wait for a task that cannot run.
 *
 * The schema is fixed by the first batch, and `get_schema` waits for it. When
 * @ref dataframe::LazyFrame::has_undeclared_columns is false, every later
 * batch is handed out as it is, and one whose columns differ from the first
 * fails the stream with `EIO`; a plan with no batch reports the columns of
 * @ref dataframe::LazyFrame::output_schema. Otherwise every batch is reshaped
 * into the first one's columns plus an `_extra` column: a column a later batch
 * adds, and a column whose type changes, is JSON-encoded there, and a column a
 * batch lacks is null, so the stream schema never changes; a plan with no batch
 * then reports a single `_extra` column.
 *
 * The errors, the end of stream and the failure behavior are those of
 * @ref to_arrow_stream(const dataframe::Series&, ArrowArrayStream*).
 */
dstd::Result<void> to_arrow_stream(dataframe::LazyFrame plan, Runtime& runtime,
                                   ArrowArrayStream* out);

/**
 * @brief Imports every batch of `in` as a Series, one chunk per batch, and
 * takes ownership of `in`: this call releases it exactly once, on every path.
 *
 * A stream that ends with no batch gives an empty Series of the stream type.
 * An InvalidArgument error for a null `in` and for a type the engine cannot
 * hold.
 */
dstd::Result<dataframe::Series> series_from_arrow_stream(ArrowArrayStream* in);

/**
 * @brief Imports every struct batch of `in` as a DataFrame, one chunk per
 * batch per column, and takes ownership of `in`: this call releases it exactly
 * once, on every path.
 *
 * A stream that ends with no batch gives a frame of no rows with the stream
 * columns. An InvalidArgument error for a null `in` and for a batch that is
 * not a struct or whose columns differ from the first batch's. A NotFound
 * error for a column type the engine cannot hold.
 */
dstd::Result<dataframe::DataFrame> dataframe_from_arrow_stream(
    ArrowArrayStream* in);

/**
 * @brief Imports `in` as a lazy plan whose scan pulls one batch per morsel
 * from the stream, and takes ownership of `in`: this call releases it exactly
 * once, on every path, with the scan that runs it.
 *
 * `get_next` runs only when the engine pulls, so nothing is read before
 * collect(). The schema comes from `get_schema`, without reading a batch; a
 * column whose Arrow type has no engine type is Unknown. The stream backs one
 * scan only: a second scan, and a second collect of the same plan, is an
 * InvalidArgument error.
 *
 * An InvalidArgument error for a null `in`, for a stream that is not a struct
 * stream, and for one whose `get_schema` fails. The cursor calls `get_next` on
 * the thread that pulls it, so a producer that cannot make progress there
 * stalls the engine.
 */
dstd::Result<dataframe::LazyFrame> lazyframe_from_arrow_stream(
    ArrowArrayStream* in);

}  // namespace dftracer::utils::arrow

#endif  // DFTRACER_UTILS_ARROW_STREAM_H
