#ifndef DFTRACER_UTILS_UTILITIES_COMMON_ARROW_FRAME_OPS_H
#define DFTRACER_UTILS_UTILITIES_COMMON_ARROW_FRAME_OPS_H

#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/utilities/common/arrow/gapfill.h>
#include <dftracer/utils/utilities/common/arrow/join.h>
#include <dftracer/utils/utilities/common/arrow/window.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::common::arrow {

/// One window column by column NAME, the DataFrame-level form of WindowSpec.
struct WindowColumn {
    WindowFunc func;
    std::optional<std::string> value;
    std::optional<std::string> time;
    std::string out;
    std::int64_t offset = 0;
    double threshold = 0.0;
    bool counter = false;
    std::int64_t preceding = 0;
    std::int64_t following = 0;
};

/// SQL window functions over `df` (the contract of window() in window.h): every
/// input column, then one column per spec, in (partition, order) sorted order.
/// Throws std::out_of_range on an unknown column name, DFTUtilsException as the
/// kernel does.
dataframe::DataFrame window(const dataframe::DataFrame& df,
                            const std::vector<std::string>& partition_by,
                            const std::vector<std::string>& order_by,
                            const std::vector<WindowColumn>& specs);

/// A regular time grid of width `bucket` over `df` (the contract of gap_fill()
/// in gapfill.h); `range` is the explicit [start, end] every partition spans,
/// or nullopt for each partition's own extent.
dataframe::DataFrame gap_fill(
    const dataframe::DataFrame& df,
    const std::vector<std::string>& partition_by, const std::string& time,
    std::int64_t bucket, const std::vector<std::string>& values,
    GapFillMode mode,
    std::optional<std::pair<std::int64_t, std::int64_t>> range);

/// As-of join of `left` to `right` on the time column `on` within the `by`
/// partition (the contract of asof_join() in join.h); `tolerance` bounds |left
/// - right|.
dataframe::DataFrame asof(const dataframe::DataFrame& left,
                          const dataframe::DataFrame& right,
                          const std::string& on,
                          const std::vector<std::string>& by,
                          AsofDirection direction,
                          std::optional<std::int64_t> tolerance);

/// Point-in-range join of `left.point` into `right.[lo, hi]` within the `by`
/// partition (the contract of interval_join() in join.h); `outer` keeps
/// unmatched left rows.
dataframe::DataFrame interval(const dataframe::DataFrame& left,
                              const dataframe::DataFrame& right,
                              const std::string& point, const std::string& lo,
                              const std::string& hi,
                              const std::vector<std::string>& by, bool outer);

/// Register dftu.frame.window / gap_fill / asof / interval into the op
/// registry. Idempotent; run from the utilities initializer alongside
/// register_host_ops().
void register_frame_ops();

}  // namespace dftracer::utils::utilities::common::arrow

extern "C" {

/** SQL window functions over `df`: PARTITION BY `partition_by` (n_part
 * names), ORDER BY `order_by` (n_order names), one appended column per spec.
 * NULL on a NULL handle, an unknown column or an unsupported type. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_window(
    const dftu_dataframe* df, const char* const* partition_by, int32_t n_part,
    const char* const* order_by, int32_t n_order, const dftu_window_spec* specs,
    int32_t n_specs);

/** A regular grid of width `bucket` on the integer `time` column, PARTITION
 * BY `partition_by`; `values` (n_values names) fill on generated rows per
 * `mode`. `range` is either NULL / n_range 0 (each partition's own extent) or
 * two int64 [start, end]. NULL on a NULL handle, an unknown column, a bad
 * `mode` or an n_range other than 0 or 2. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_gap_fill(
    const dftu_dataframe* df, const char* const* partition_by, int32_t n_part,
    const char* time, int64_t bucket, const char* const* values,
    int32_t n_values, dftu_gap_fill_mode mode, const int64_t* range,
    int32_t n_range);

/** As-of join of `left` to `right` on the time column `on` within the `by`
 * partition (n_by names); a negative `tolerance` means unbounded. NULL on a
 * NULL handle, an unknown column, a bad `direction` or a type mismatch. */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_asof(
    const dftu_dataframe* left, const dftu_dataframe* right, const char* on,
    const char* const* by, int32_t n_by, dftu_asof_direction direction,
    int64_t tolerance);

/** Point-in-range join of `left.point` into `right.[lo, hi]` within the `by`
 * partition (n_by names); nonzero `outer` keeps unmatched left rows with null
 * right values. NULL on a NULL handle, an unknown column or a type mismatch.
 */
DFTU_EXPORT dftu_dataframe* dftu_dataframe_interval(
    const dftu_dataframe* left, const dftu_dataframe* right, const char* point,
    const char* lo, const char* hi, const char* const* by, int32_t n_by,
    int32_t outer);
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW
#endif  // DFTRACER_UTILS_UTILITIES_COMMON_ARROW_FRAME_OPS_H
