#ifndef DFTRACER_UTILS_DATAFRAME_LAZY_CURSORS_H
#define DFTRACER_UTILS_DATAFRAME_LAZY_CURSORS_H

#include <dftracer/utils/dataframe/lazy/frame_op.h>

namespace dftracer::utils::dataframe::lazy_internal {

std::unique_ptr<Cursor> make_sort_merge(std::unique_ptr<Cursor> in,
                                        std::vector<std::string> sch,
                                        std::vector<std::string> keys,
                                        std::vector<bool> descending,
                                        std::uint64_t budget);

std::unique_ptr<Cursor> make_group_by(std::unique_ptr<Cursor> in,
                                      std::vector<std::string> sch,
                                      std::vector<std::string> keys,
                                      std::vector<GroupAgg> aggs,
                                      std::uint64_t budget,
                                      std::vector<AggDynSpec> dyn = {},
                                      std::string dyn_prefix = {});

std::unique_ptr<Cursor> make_group_by_dynamic(
    std::unique_ptr<Cursor> in, std::vector<std::string> sch,
    std::string time_col, std::int64_t every, std::int64_t period,
    std::vector<GroupAgg> aggs, std::int64_t origin, bool origin_min,
    std::uint64_t budget);

std::unique_ptr<Cursor> make_head_by(std::unique_ptr<Cursor> in,
                                     std::vector<std::int64_t> key_idx,
                                     std::int64_t n, std::uint64_t budget);

std::unique_ptr<Cursor> make_unique_rows(std::unique_ptr<Cursor> in,
                                         std::vector<std::string> sch,
                                         std::vector<std::int64_t> key_idx,
                                         std::uint64_t budget);

std::unique_ptr<Cursor> make_pivot(std::unique_ptr<Cursor> in,
                                   std::uint64_t budget,
                                   std::vector<std::string> sch,
                                   std::string index, std::string on,
                                   std::string values, std::string agg);

std::unique_ptr<Cursor> make_compare_agg(std::unique_ptr<Cursor> in,
                                         std::vector<std::string> sch,
                                         LazyFrame other, std::int64_t n_key,
                                         std::uint64_t budget);

LazyFrame compose_compare_agg(const LazyFrame& base, const LazyFrame& variant,
                              const std::vector<Field>& lhs,
                              const std::vector<Field>& rhs, std::size_t nk);

// `in_fields` types the input columns (empty or Unknown when the plan cannot
// say): a float sum over a whole partition depends on the order it adds in.
std::unique_ptr<Cursor> make_window(
    std::unique_ptr<Cursor> in, const std::vector<std::string>& sch,
    const std::vector<Field>& in_fields, const std::string& name,
    const dftu_op_desc* op, std::shared_ptr<const OwnedFrameOpArgs> args,
    std::uint64_t budget);

// The streaming as-of join (`name` is ASOF_OP) or interval join (INTERVAL_OP)
// of `in` with the plan `other`, which already carries the plan's budget. Both
// sides are sorted by (by, time) with the spilling sort and merged, holding
// only the by-group window the merge needs. Null, leaving `in` untouched, when
// a side's columns are only known once it runs, so the caller collects both
// sides instead.
std::unique_ptr<Cursor> make_ordered_join(std::unique_ptr<Cursor>& in,
                                          const std::vector<std::string>& sch,
                                          const std::string& name,
                                          const OwnedFrameOpArgs& args,
                                          const LazyFrame& other,
                                          std::uint64_t budget);

// The plan steps of LazyFrame::asof and LazyFrame::interval: checks what the
// declared schemas settle, then appends the registry op that make_ordered_join
// streams.
LazyFrame compose_asof(const LazyFrame& left, LazyFrame other,
                       const std::string& on,
                       const std::vector<std::string>& by,
                       AsofDirection direction,
                       std::optional<double> tolerance);

LazyFrame compose_interval(const LazyFrame& left, LazyFrame other,
                           const std::string& point, const std::string& lo,
                           const std::string& hi,
                           const std::vector<std::string>& by, bool outer);

std::unique_ptr<Cursor> make_native_transform(
    std::unique_ptr<Cursor> in, const std::vector<std::string>& sch,
    const OwnedFrameOpArgs& args, const std::vector<std::string>& out_names,
    const std::vector<DataType>& out_types, std::uint64_t budget);

// The streaming form of the column op `args` (COLUMN_OP): a running or
// positional op (cumsum, cum_prod, cummax, cummin, ffill, diff, shift) over one
// integer or, for ffill, diff and shift, numeric column, run one morsel at a
// time with the rows the next morsel needs carried over, so the result equals
// the op over the whole column. Null, leaving `in` untouched, when the op,
// the column or its type is not covered; the caller then runs the op over the
// whole input.
std::unique_ptr<Cursor> make_column_scan(std::unique_ptr<Cursor>& in,
                                         const std::vector<std::string>& sch,
                                         const std::vector<Field>& in_fields,
                                         const OwnedFrameOpArgs& args);

// The streaming form of the frame op with_column (WITH_COLUMN_OP): each morsel
// gets the rows of the operand column that match it. Null, leaving `in`
// untouched, when the input's columns are only known once it runs.
std::unique_ptr<Cursor> make_series_column(std::unique_ptr<Cursor>& in,
                                           const std::vector<std::string>& sch,
                                           const std::vector<Field>& in_fields,
                                           const OwnedFrameOpArgs& args);

}  // namespace dftracer::utils::dataframe::lazy_internal

#endif  // DFTRACER_UTILS_DATAFRAME_LAZY_CURSORS_H
