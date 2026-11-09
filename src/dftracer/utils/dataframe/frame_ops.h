// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_FRAME_OPS_H
#define DFTRACER_UTILS_DATAFRAME_FRAME_OPS_H

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace dftracer::utils::dataframe {

/// Sentinel for an unbounded FRAME_* bound; in Range mode an unbounded value
/// range on that side.
inline constexpr std::int64_t WINDOW_UNBOUNDED =
    (std::numeric_limits<std::int64_t>::max)();

/// The parameters only some window functions read; `WindowColumn::func` names
/// the member that is set, and only that one may be read.
union WindowParams {
    /// Lag/Lead shift, Ntile bucket count, NthValue 1-based k.
    std::int64_t offset;
    /// Frame*: the present count below which the output is null (0 = none)
    /// and the bounds (WINDOW_UNBOUNDED = unbounded that side).
    struct Frame {
        std::int64_t min_count;
        std::int64_t preceding;
        std::int64_t following;
        WindowFrameMode mode;
        /// FrameQuantile: the level in [0, 1].
        double q;
    } frame;
    /// Rate: the counter-reset correction.
    struct Rate {
        bool counter;
    } rate;
    /// Sessionize: the gap and the longest session (0 = no limit), in the
    /// time's unit.
    struct Session {
        double gap;
        double span;
    } session;
};

/// One appended window column by column name. value() names the column the
/// function reads (none for ranking functions, Ntile and Sessionize), time()
/// the Rate and Sessionize time column and end() the Sessionize row-end column
/// (absent: each row ends at its time). The three names share two slots keyed
/// by `func`: set `func` before a name. A name is absent when empty.
struct WindowColumn {
    WindowFunc func;
    dstd::String out;
    WindowParams params{};

    const dstd::String* value() const {
        return func == WindowFunc::Sessionize ? nullptr : named(0);
    }
    const dstd::String* time() const {
        if (func == WindowFunc::Rate) return named(1);
        return func == WindowFunc::Sessionize ? named(0) : nullptr;
    }
    const dstd::String* end() const {
        return func == WindowFunc::Sessionize ? named(1) : nullptr;
    }
    /// The FrameArgMax/FrameArgMin ordering column.
    const dstd::String* by() const {
        return func == WindowFunc::FrameArgMax ||
                       func == WindowFunc::FrameArgMin
                   ? named(1)
                   : nullptr;
    }
    void set_by(dstd::String name) {
        if (func == WindowFunc::FrameArgMax || func == WindowFunc::FrameArgMin)
            names_[1] = std::move(name);
    }
    void set_value(dstd::String name) {
        if (func != WindowFunc::Sessionize) names_[0] = std::move(name);
    }
    void set_time(dstd::String name) {
        if (func == WindowFunc::Rate) names_[1] = std::move(name);
        if (func == WindowFunc::Sessionize) names_[0] = std::move(name);
    }
    void set_end(dstd::String name) {
        if (func == WindowFunc::Sessionize) names_[1] = std::move(name);
    }

   private:
    const dstd::String* named(int i) const {
        return names_[i].empty() ? nullptr : &names_[i];
    }
    dstd::String names_[2];
};

/// Whether `f` is a Frame* function (reads WindowParams::frame).
constexpr bool is_frame_func(WindowFunc f) {
    return f == WindowFunc::FrameSum || f == WindowFunc::FrameMin ||
           f == WindowFunc::FrameMax || f == WindowFunc::FrameCount ||
           f == WindowFunc::FrameMean || f == WindowFunc::FrameVar ||
           f == WindowFunc::FrameStd || f == WindowFunc::FrameQuantile ||
           f == WindowFunc::FrameCountDistinct ||
           f == WindowFunc::FrameArgMax || f == WindowFunc::FrameArgMin ||
           f == WindowFunc::FrameCollect;
}

/// Whether `f` reads WindowParams::offset.
constexpr bool is_offset_func(WindowFunc f) {
    return f == WindowFunc::Lag || f == WindowFunc::Lead ||
           f == WindowFunc::Ntile || f == WindowFunc::NthValue;
}

/// The @ref WindowColumn a C ABI window spec names; its strings are copied.
WindowColumn window_column(const dftu_window_spec& s);

/// SQL window functions over `df`, PARTITION BY `partition_by` and ORDER BY
/// `order_by` (either may be empty). The output holds every input column,
/// then one column per spec, in sorted (partition, order) order with ties in
/// row order and nulls last. Each spec's type follows its function and value
/// column. Frame* default to Rows framing; Range framing needs one numeric
/// order column and includes equal-order peers. Running and Lag/Lead read a
/// null source cell as null; an all-null frame aggregate is null. An unknown
/// column is a NotFound error; an unsupported column type, an invalid
/// parameter or an integer sum or delta past its type is an InvalidArgument
/// error. A FrameCollect whose frames hold more than 2^27 values in all is
/// fatal.
dstd::Result<DataFrame> window(const DataFrame& df,
                               const dstd::Vector<dstd::String>& partition_by,
                               const dstd::Vector<dstd::String>& order_by,
                               const dstd::Vector<WindowColumn>& specs);

/// One row per grid point per partition of width `bucket` over the integer or
/// floating `time` column: a real row that lands on a grid point is kept, one
/// is generated where none does. Each partition's grid spans its real rows
/// floor-aligned to `bucket`, or the floor-aligned `range` when given. A
/// generated row carries the partition and time values, `values` filled per
/// `mode` (None: null; Locf: the last real value, a leading gap null; Linear:
/// interpolation as Float64, no extrapolation), and nulls elsewhere.
/// Duplicate real times in one bucket keep the first. A row with a null time
/// is dropped, and a partition with no real time gives no rows; with `range`
/// real rows outside it are dropped but still neighbor the Locf and Linear
/// fill. A non-positive width, a Linear fill over a non-numeric column or a
/// grid over 10,000,000 points is an InvalidArgument error, and an unknown
/// column is a NotFound error.
dstd::Result<DataFrame> gap_fill(
    const DataFrame& df, const dstd::Vector<dstd::String>& partition_by,
    const dstd::String& time, std::int64_t bucket,
    const dstd::Vector<dstd::String>& values, GapFillMode mode,
    dstd::Optional<dstd::Pair<std::int64_t, std::int64_t>> range);

/// Matches each `left` row to the one `right` row nearest in the numeric time
/// column `on` (in both frames) within the `by` partition: Backward the
/// largest time not after it, Forward the smallest not before it, Nearest the
/// closest (equal distance: the earlier). Among right rows with equal times
/// Backward takes the last and Forward the first; Nearest takes the last at
/// or before the left time and the first after it. Every left row appears; a
/// null or NaN time, no candidate or a match farther than `tolerance` (in the
/// units of `on`; a fraction bounds integer times by its floor) gives
/// null right values, and a null or NaN right time is never a candidate. A
/// null key matches only a null key (pandas merge_asof). The output holds
/// every left column, then the right columns but `on` and `by` (a colliding
/// name gets "_right"),
/// rows ordered by (by, time) with ties in left row order. Non-numeric or
/// unequal time or key types are an InvalidArgument error, and an unknown
/// column is a NotFound error.
dstd::Result<DataFrame> asof(const DataFrame& left, const DataFrame& right,
                             const dstd::String& on,
                             const dstd::Vector<dstd::String>& by,
                             AsofDirection direction,
                             dstd::Optional<double> tolerance);

/// Matches each `left.point` to every `right` row whose closed interval
/// `[lo, hi]` contains it within the `by` partition, one output row per pair
/// ordered by (by, point) then (lo, hi, right row). `outer` keeps an
/// unmatched left row once with null right values. A null or NaN point or
/// bound never matches; a null key matches only a null key. The output holds
/// every left column, then the right columns but `lo`, `hi` and `by` (a
/// colliding name gets "_right"). Non-numeric or unequal types are an
/// InvalidArgument error, and an unknown column is a NotFound error.
dstd::Result<DataFrame> interval(const DataFrame& left, const DataFrame& right,
                                 const dstd::String& point,
                                 const dstd::String& lo, const dstd::String& hi,
                                 const dstd::Vector<dstd::String>& by,
                                 bool outer);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_FRAME_OPS_H
