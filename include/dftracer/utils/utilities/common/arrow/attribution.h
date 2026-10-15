#ifndef DFTRACER_UTILS_UTILITIES_COMMON_ARROW_ATTRIBUTION_H
#define DFTRACER_UTILS_UTILITIES_COMMON_ARROW_ATTRIBUTION_H

#include <dftracer/utils/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/utilities/common/arrow/arrow_export.h>

#include <cstdint>

namespace dftracer::utils::utilities::common::arrow {

/// Rank dimension values over-represented in a selection vs baseline (Honeycomb
/// BubbleUp). `select_col` is BOOL: true selects, false is baseline, null
/// skips; `top_k` > 0 caps the output. Emits [dimension, value (both STRING),
/// selected_count, baseline_count (INT64), selected_frac, baseline_frac,
/// difference (DOUBLE = selected_frac - baseline_frac)], one row per (dim,
/// distinct value), sorted by |difference| desc then (dimension, value). Dim
/// values render to string and nulls skip. Throws on a bad index or type.
ArrowExportResult attribute(const ArrowSchema* s, const ArrowArray* a,
                            std::uint32_t select_col,
                            const std::uint32_t* dim_cols, std::uint32_t n_dim,
                            std::uint32_t top_k);

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW
#endif  // DFTRACER_UTILS_UTILITIES_COMMON_ARROW_ATTRIBUTION_H
