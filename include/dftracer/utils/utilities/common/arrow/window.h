#ifndef DFTRACER_UTILS_UTILITIES_COMMON_ARROW_WINDOW_H
#define DFTRACER_UTILS_UTILITIES_COMMON_ARROW_WINDOW_H

#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/utilities/common/arrow/arrow_export.h>

#include <cstdint>
#include <limits>
#include <string>

namespace dftracer::utils::utilities::common::arrow {

enum class WindowFunc {
    ROW_NUMBER,
    RANK,
    DENSE_RANK,
    LAG,
    LEAD,
    RUNNING_SUM,
    RUNNING_MIN,
    RUNNING_MAX,
    RUNNING_COUNT,
    DELTA,
    RATE,
    SESSIONIZE,
    FRAME_SUM,
    FRAME_MIN,
    FRAME_MAX,
    FRAME_COUNT,
    FRAME_MEAN,
    NTILE,
    FIRST_VALUE,
    LAST_VALUE,
    NTH_VALUE,
    PERCENT_RANK,
    CUME_DIST,
    FILL_FORWARD,
    RUNNING_PROD
};

/// FRAME_* bound interpretation: ROWS = row offsets (default), RANGE = value
/// deltas on the single numeric order column.
enum class WindowFrameMode { ROWS, RANGE };

/// Sentinel for an UNBOUNDED frame bound; in RANGE mode an unbounded value
/// range on that side.
inline constexpr std::int64_t WINDOW_UNBOUNDED =
    (std::numeric_limits<std::int64_t>::max)();

/// One appended output column: `func` over `value_col`/`time_col`, with
/// `offset` (LAG/LEAD shift, NTILE bucket count, NTH_VALUE 1-based k, or the
/// FRAME_SUM/MIN/MAX/MEAN minimum present count below which the output is
/// null; 0 = none),
/// `threshold` (SESSIONIZE gap), `counter` (RATE reset correction), and
/// `frame_preceding`/ `frame_following` (FRAME_* bounds; WINDOW_UNBOUNDED =
/// unbounded that side).
struct WindowSpec {
    WindowFunc func;
    std::uint32_t value_col;
    std::int64_t offset;
    std::string name;
    std::uint32_t time_col = 0;
    double threshold = 0.0;
    bool counter = false;
    std::int64_t frame_preceding = 0;
    std::int64_t frame_following = 0;
    WindowFrameMode frame_mode = WindowFrameMode::ROWS;
};

/// SQL window functions over one materialized batch, PARTITION BY
/// `partition_cols` and ORDER BY `order_cols` (either count may be 0). Output
/// carries all input columns through, then one column per spec, in sorted
/// (partition, order) order with ties broken by original row index (sort-column
/// nulls last). Each spec's output type follows its function and value column.
/// FRAME_* default to ROWS framing; RANGE framing needs a single numeric order
/// column and includes equal-order peers. Cumulative and LAG/LEAD read a null
/// source cell as null, an all-null frame aggregate is null. Throws
/// DFTUtilsException on an out-of-range index, an unsupported column type, or a
/// RUNNING/FRAME aggregate over a non-numeric column.
ArrowExportResult window(const ArrowSchema* s, const ArrowArray* a,
                         const std::uint32_t* partition_cols,
                         std::uint32_t n_part, const std::uint32_t* order_cols,
                         std::uint32_t n_order, const WindowSpec* specs,
                         std::uint32_t n_spec);

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW
#endif  // DFTRACER_UTILS_UTILITIES_COMMON_ARROW_WINDOW_H
