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

/// The parameters only some window functions read; `func` of the owning
/// WindowSpec names the member that is set, and only that one may be read.
union WindowParams {
    /// LAG/LEAD shift, NTILE bucket count, NTH_VALUE 1-based k.
    std::int64_t offset;
    /// FRAME_*: the present count below which the output is null (0 = none)
    /// and the bounds (WINDOW_UNBOUNDED = unbounded that side).
    struct Frame {
        std::int64_t min_count;
        std::int64_t preceding;
        std::int64_t following;
        WindowFrameMode mode;
    } frame;
    /// RATE: the time column and the counter-reset correction.
    struct Rate {
        std::uint32_t time_col;
        bool counter;
    } rate;
    /// SESSIONIZE: the time column, the row-end column (used when `has_end`),
    /// the gap and the longest session (0 = no limit), in the time's unit.
    struct Session {
        std::uint32_t time_col;
        std::uint32_t end_col;
        bool has_end;
        double gap;
        double span;
    } session;
};

/// One appended output column: `func` over `value_col`, with the
/// parameters its function reads.
struct WindowSpec {
    WindowFunc func;
    std::uint32_t value_col = 0;
    std::string name;
    WindowParams params{};
};

/// Whether `f` is a FRAME_* function (reads WindowParams::frame).
constexpr bool is_frame_func(WindowFunc f) {
    return f == WindowFunc::FRAME_SUM || f == WindowFunc::FRAME_MIN ||
           f == WindowFunc::FRAME_MAX || f == WindowFunc::FRAME_COUNT ||
           f == WindowFunc::FRAME_MEAN;
}

/// Whether `f` reads WindowParams::offset.
constexpr bool is_offset_func(WindowFunc f) {
    return f == WindowFunc::LAG || f == WindowFunc::LEAD ||
           f == WindowFunc::NTILE || f == WindowFunc::NTH_VALUE;
}

/// A spec of a function that reads no parameters, or reads
/// WindowParams::offset (`offset` is ignored by the others). Use frame_spec,
/// rate_spec or session_spec for the functions with their own parameters.
inline WindowSpec window_spec(WindowFunc func, std::uint32_t value_col,
                              std::string name, std::int64_t offset = 0) {
    WindowSpec s{func, value_col, std::move(name), {}};
    if (is_offset_func(func)) s.params.offset = offset;
    return s;
}

/// A FRAME_* spec.
inline WindowSpec frame_spec(WindowFunc func, std::uint32_t value_col,
                             std::string name, std::int64_t preceding,
                             std::int64_t following, std::int64_t min_count = 0,
                             WindowFrameMode mode = WindowFrameMode::ROWS) {
    WindowSpec s{func, value_col, std::move(name), {}};
    s.params.frame = {min_count, preceding, following, mode};
    return s;
}

/// A RATE spec.
inline WindowSpec rate_spec(std::uint32_t value_col, std::uint32_t time_col,
                            std::string name, bool counter = false) {
    WindowSpec s{WindowFunc::RATE, value_col, std::move(name), {}};
    s.params.rate = {time_col, counter};
    return s;
}

/// A SESSIONIZE spec; `end_col` of UINT32_MAX means each row ends at its time.
inline WindowSpec session_spec(
    std::uint32_t time_col, std::string name, double gap, double span = 0.0,
    std::uint32_t end_col = (std::numeric_limits<std::uint32_t>::max)()) {
    WindowSpec s{WindowFunc::SESSIONIZE, 0, std::move(name), {}};
    const bool has_end = end_col != (std::numeric_limits<std::uint32_t>::max)();
    s.params.session = {time_col, has_end ? end_col : 0, has_end, gap, span};
    return s;
}

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
