// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_LAZY_CURSORS_H
#define DFTRACER_UTILS_DATAFRAME_LAZY_CURSORS_H

#include <dftracer/utils/dataframe/lazy/frame_op.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

namespace dftracer::utils::dataframe::lazy_internal {

dstd::UniquePtr<Cursor> make_sort_merge(dstd::UniquePtr<Cursor> in,
                                        dstd::Vector<dstd::String> sch,
                                        dstd::Vector<dstd::String> keys,
                                        dstd::Vector<bool> descending,
                                        std::uint64_t budget);

dstd::UniquePtr<Cursor> make_group_by(dstd::UniquePtr<Cursor> in,
                                      dstd::Vector<dstd::String> sch,
                                      dstd::Vector<dstd::String> keys,
                                      dstd::Vector<GroupAgg> aggs,
                                      std::uint64_t budget,
                                      dstd::Vector<AggDynSpec> dyn = {},
                                      dstd::String dyn_prefix = {});

dstd::UniquePtr<Cursor> make_group_by_dynamic(
    dstd::UniquePtr<Cursor> in, dstd::Vector<dstd::String> sch,
    dstd::String time_col, std::int64_t every, std::int64_t period,
    dstd::Vector<GroupAgg> aggs, std::int64_t origin, bool origin_min,
    std::uint64_t budget);

dstd::UniquePtr<Cursor> make_head_by(dstd::UniquePtr<Cursor> in,
                                     dstd::Vector<std::int64_t> key_idx,
                                     std::int64_t n, std::uint64_t budget);

dstd::UniquePtr<Cursor> make_unique_rows(dstd::UniquePtr<Cursor> in,
                                         dstd::Vector<dstd::String> sch,
                                         dstd::Vector<std::int64_t> key_idx,
                                         std::uint64_t budget);

dstd::UniquePtr<Cursor> make_pivot(dstd::UniquePtr<Cursor> in,
                                   std::uint64_t budget,
                                   dstd::Vector<dstd::String> sch,
                                   dstd::String index, dstd::String on,
                                   dstd::String values, dstd::String agg);

dstd::UniquePtr<Cursor> make_compare_agg(dstd::UniquePtr<Cursor> in,
                                         dstd::Vector<dstd::String> sch,
                                         LazyFrame other, std::int64_t n_key,
                                         std::uint64_t budget);

LazyFrame compose_compare_agg(const LazyFrame& base, const LazyFrame& variant,
                              const dstd::Vector<Field>& lhs,
                              const dstd::Vector<Field>& rhs, std::size_t nk);

// `in_fields` types the input columns (empty or Unknown when the plan cannot
// say): a float sum over a whole partition depends on the order it adds in.
dstd::UniquePtr<Cursor> make_window(
    dstd::UniquePtr<Cursor> in, const dstd::Vector<dstd::String>& sch,
    const dstd::Vector<Field>& in_fields, const dstd::String& name,
    const dftu_op_desc* op, dstd::SharedPtr<const OwnedFrameOpArgs> args,
    std::uint64_t budget);

// The streaming as-of join (`name` is ASOF_OP) or interval join (INTERVAL_OP)
// of `in` with the plan `other`, which already carries the plan's budget. Both
// sides are sorted by (by, time) with the spilling sort and merged, holding
// only the by-group window the merge needs. Null, leaving `in` untouched, when
// a side's columns are only known once it runs, so the caller collects both
// sides instead.
dstd::UniquePtr<Cursor> make_ordered_join(dstd::UniquePtr<Cursor>& in,
                                          const dstd::Vector<dstd::String>& sch,
                                          const dstd::String& name,
                                          const OwnedFrameOpArgs& args,
                                          const LazyFrame& other,
                                          std::uint64_t budget);

// The plan steps of LazyFrame::asof and LazyFrame::interval: checks what the
// declared schemas settle, then appends the registry op that make_ordered_join
// streams.
LazyFrame compose_asof(const LazyFrame& left, LazyFrame other,
                       const dstd::String& on,
                       const dstd::Vector<dstd::String>& by,
                       AsofDirection direction,
                       dstd::Optional<double> tolerance);

LazyFrame compose_interval(const LazyFrame& left, LazyFrame other,
                           const dstd::String& point, const dstd::String& lo,
                           const dstd::String& hi,
                           const dstd::Vector<dstd::String>& by, bool outer);

dstd::UniquePtr<Cursor> make_native_transform(
    dstd::UniquePtr<Cursor> in, const dstd::Vector<dstd::String>& sch,
    const OwnedFrameOpArgs& args, const dstd::Vector<dstd::String>& out_names,
    const dstd::Vector<DataType>& out_types, std::uint64_t budget);

// The streaming form of the column op `args` (COLUMN_OP): a running or
// positional op (cumsum, cum_prod, cummax, cummin, ffill, diff, shift) over one
// integer or, for ffill, diff and shift, numeric column, run one morsel at a
// time with the rows the next morsel needs carried over, so the result equals
// the op over the whole column. Null, leaving `in` untouched, when the op,
// the column or its type is not covered; the caller then runs the op over the
// whole input.
dstd::UniquePtr<Cursor> make_column_scan(dstd::UniquePtr<Cursor>& in,
                                         const dstd::Vector<dstd::String>& sch,
                                         const dstd::Vector<Field>& in_fields,
                                         const OwnedFrameOpArgs& args);

// The streaming form of the frame op with_column (WITH_COLUMN_OP): each morsel
// gets the rows of the operand column that match it. Null, leaving `in`
// untouched, when the input's columns are only known once it runs.
dstd::UniquePtr<Cursor> make_series_column(
    dstd::UniquePtr<Cursor>& in, const dstd::Vector<dstd::String>& sch,
    const dstd::Vector<Field>& in_fields, const OwnedFrameOpArgs& args);

}  // namespace dftracer::utils::dataframe::lazy_internal

#endif  // DFTRACER_UTILS_DATAFRAME_LAZY_CURSORS_H
