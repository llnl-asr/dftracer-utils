#ifndef DFTRACER_UTILS_UTILITIES_COMMON_ARROW_JOIN_H
#define DFTRACER_UTILS_UTILITIES_COMMON_ARROW_JOIN_H

#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/utilities/common/arrow/arrow_export.h>

#include <cstdint>

namespace dftracer::utils::utilities::common::arrow {

enum class JoinType { INNER, LEFT, RIGHT, FULL, LEFT_SEMI, LEFT_ANTI };

/// Same-key-schema equi sort-merge join of two materialized record batches; the
/// caller retains ownership of both. `left_key_cols`/`right_key_cols` are the
/// nkey (>=1) positionally-paired join key columns; paired columns must share
/// Arrow storage type. Output is [join key columns (left names/types), left
/// value columns, right value columns]; a colliding right value name is
/// suffixed
/// "_right" (a second collision throws), and rows are ordered by join key then
/// (left, right) row order. A null key never matches (SQL semantics), so it
/// appears only in the outer (LEFT/RIGHT/FULL) output, null-padded on the other
/// side. LEFT_SEMI/LEFT_ANTI emit a left-only schema: SEMI each >=1-matched
/// left row once (no cross-product), ANTI each unmatched left row. Value
/// columns pass through by value: scalars and
/// `list<utf8>`/`list<int64>`/`list<struct<...>>`. Throws DFTUtilsException on
/// nkey==0, an out-of-range key index, mismatched key types, or an unsupported
/// column type.
ArrowExportResult join(const ArrowSchema* left_s, const ArrowArray* left_a,
                       const std::uint32_t* left_key_cols,
                       const ArrowSchema* right_s, const ArrowArray* right_a,
                       const std::uint32_t* right_key_cols, std::uint32_t nkey,
                       JoinType type);

enum class AsofDirection { BACKWARD, FORWARD, NEAREST };

/// ASOF (temporal) join: match each LEFT row to the single most-recent RIGHT
/// row by a time key, within an optional equi-key partition; every left row
/// appears (unmatched -> null right values). `left_ts_col`/`right_ts_col` must
/// share a numeric Arrow storage type; `left_equi_cols`/`right_equi_cols` are
/// n_equi positionally-paired partition columns (n_equi 0 = global). Direction:
/// BACKWARD matches the largest ts <= left.ts, FORWARD the smallest ts >=
/// left.ts, NEAREST the closest |left.ts - right.ts| (tie -> BACKWARD). Among
/// right rows with equal ts, BACKWARD and NEAREST take the last in row order
/// and FORWARD the first, as pandas merge_asof does. With
/// `allow_exact` false, an equal ts is excluded, making BACKWARD strictly < and
/// FORWARD strictly > (NEAREST is unaffected). With
/// `has_tol`, a match past |left.ts - right.ts| <= `tol` (ts native units) is
/// dropped to null. Output is all LEFT columns, then the RIGHT value columns
/// other than ts/equi; a colliding right name is suffixed "_right" (a second
/// collision throws), rows ordered by (equi, ts). Value columns pass through as
/// in `join`. Throws DFTUtilsException on an out-of-range index, mismatched or
/// non-numeric ts types, mismatched equi types, or an unsupported column type.
ArrowExportResult asof_join(
    const ArrowSchema* left_s, const ArrowArray* left_a,
    std::uint32_t left_ts_col, const std::uint32_t* left_equi_cols,
    const ArrowSchema* right_s, const ArrowArray* right_a,
    std::uint32_t right_ts_col, const std::uint32_t* right_equi_cols,
    std::uint32_t n_equi, AsofDirection dir, bool has_tol, std::int64_t tol,
    bool allow_exact = true);

/// INTERVAL (point-in-range) join: match each LEFT row's POINT to every RIGHT
/// row whose CLOSED interval [lo, hi] contains it (lo <= point <= hi), within
/// an optional equi-key partition; one output row per (left, matching right).
/// `left_outer` also emits an unmatched left row once with null right values;
/// `right_outer` likewise emits each right interval that covered no point once,
/// null-padded on the left (set both for a full outer join), ordered after all
/// matched rows by (equi, lo, row).
/// `left_point_col`/`right_lo_col`/`right_hi_col` must share Arrow storage
/// type; `left_equi_cols`/`right_equi_cols` are n_equi positionally-paired
/// partition columns (n_equi 0 = global). A null point/lo/hi never matches.
/// Output is all LEFT columns, then the RIGHT value columns other than
/// lo/hi/equi; a colliding right name is suffixed "_right" (a second collision
/// throws), rows ordered by left (equi, point) then (lo, hi, right row). Value
/// columns pass through as in `join`. Throws DFTUtilsException on an
/// out-of-range index, mismatched or non-numeric point/lo/hi types, mismatched
/// equi types, or an unsupported column type.
ArrowExportResult interval_join(
    const ArrowSchema* left_s, const ArrowArray* left_a,
    std::uint32_t left_point_col, const std::uint32_t* left_equi_cols,
    const ArrowSchema* right_s, const ArrowArray* right_a,
    std::uint32_t right_lo_col, std::uint32_t right_hi_col,
    const std::uint32_t* right_equi_cols, std::uint32_t n_equi, bool left_outer,
    bool right_outer = false);

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW
#endif  // DFTRACER_UTILS_UTILITIES_COMMON_ARROW_JOIN_H
