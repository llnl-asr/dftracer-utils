// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_KERNELS_STATS_H
#define DFTRACER_UTILS_DATAFRAME_KERNELS_STATS_H

#include <dftracer/utils/dataframe/dataframe.h>

#include <cstdint>

// Summary statistics over a FLAT column, skipping nulls. Moment-based stats
// (variance/stddev/skewness/kurtosis) are numeric-only; quantile/median sort
// the values (SIMD VQSort); unique/nunique work for any type via argsort.
namespace dftracer::utils::dataframe {

/// Variance of the non-null values. `sample` divides by (n-1) (Bessel), else by
/// n. NaN when there is no value, and for one value in the sample form.
double variance(const Series& v, bool sample = true);

/// Standard deviation (sqrt of variance).
double stddev(const Series& v, bool sample = true);

/// Population skewness (Fisher-Pearson g1); 0 when undefined.
double skewness(const Series& v);

/// Excess kurtosis (population; normal == 0); 0 when undefined.
double kurtosis(const Series& v);

/// The pandas sample skewness G1 and sample excess kurtosis G2: NaN below
/// three and four values and for a column with no spread.
double skewness_unbiased(const Series& v);
double kurtosis_unbiased(const Series& v);

/// The q-th quantile (q in [0, 1]) with linear interpolation between the two
/// nearest ranks. NaN when there are no non-null values. A NaN value ranks
/// above every number.
double quantile(const Series& v, double q);

/// The q-th quantile with @p interp choosing the result between two ranks.
double quantile(const Series& v, double q, Interpolation interp);

/// The q-th quantile of @p values, which are reordered and may hold NaN (ranked
/// above every number). NaN for no value or a NaN @p q; @p q is clamped to
/// [0, 1]. @p may_have_nan false skips the scan for a NaN: the caller knows
/// the values came from an integer column. The merged state of the exact
/// quantile aggregate finishes through this.
double quantile_of(dstd::Vector<double>& values, double q, Interpolation interp,
                   bool may_have_nan = true);

/// The median (quantile 0.5).
double median(const Series& v);

/// Number of distinct non-null values.
std::int64_t nunique(const Series& v);

/// The distinct values, a null kept once. In first-occurrence order when
/// `maintain_order`, otherwise ascending with the null first.
Series unique(const Series& v, bool maintain_order = false);

/// Rank of each row (ascending unless `descending`) as a Float64 column; ties
/// resolve per `method` (see RankMethod in types.h). A null row has a null
/// rank. With `pct`, each rank is divided by the count of non-null values (by
/// the number of distinct values for RankMethod::Dense, as pandas does), so the
/// ranks lie in (0, 1].
Series rank(const Series& v, RankMethod method = RankMethod::Average,
            bool descending = false, bool pct = false);

/// Rolling reduction over each row's `window` trailing values, as a Float64
/// column; the first window-1 rows are null. `op` selects the reduction (see
/// RollingOp in types.h).
Series rolling(const Series& v, std::int64_t window, RollingOp op);

/// Rolling sample variance / standard deviation over each row's `window`
/// trailing values, as a Float64 column; the first window-1 rows are null. A
/// window of fewer than 2 values yields 0.
Series rolling_var(const Series& v, std::int64_t window);
Series rolling_std(const Series& v, std::int64_t window);

/// Rolling median / quantile (`q` in [0, 1], linear interpolation) over each
/// row's `window` trailing values, as a Float64 column; the first window-1 rows
/// are null.
Series rolling_median(const Series& v, std::int64_t window);
Series rolling_quantile(const Series& v, std::int64_t window, double q);

/// Exponentially weighted mean with smoothing `alpha` in (0, 1]
/// (y[0]=x[0], y[i]=alpha*x[i]+(1-alpha)*y[i-1]), as a Float64 column. A
/// sequential scan.
Series ewm_mean(const Series& v, double alpha);

/// Exponentially weighted sample standard deviation with smoothing `alpha` in
/// (0, 1] (reliability-weight debiased), as a Float64 column; row 0 (variance
/// undefined) is null. A sequential scan.
Series ewm_std(const Series& v, double alpha);

/// Bin each value into the half-open intervals defined by the ascending numeric
/// `breaks`: bin index = count of breaks <= x, in 0..breaks.length(). Int32
/// column; a null input row yields a null bin.
Series cut(const Series& v, const Series& breaks, std::int32_t flags = 0);

/// Like `cut`, but the edges are the `q`-quantiles of the column (q buckets,
/// q-1 interior edges), so bins are 0..q-1. Int32 column; a null input row
/// yields a null bin.
Series qcut(const Series& v, std::int32_t q);

/// For each element of `values`, the lower-bound insertion index into THIS
/// (assumed sorted ascending) column that keeps it sorted. Int64 column of
/// length values.length().
Series search_sorted(const Series& v, const Series& values, bool right = false);

/// Linearly interpolate null interior values between their nearest non-null
/// neighbors (by row index); leading and trailing nulls stay null. Float64
/// column.
Series interpolate(const Series& v);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_KERNELS_STATS_H
