#ifndef DFTRACER_UTILS_DATAFRAME_KERNELS_STATS_H
#define DFTRACER_UTILS_DATAFRAME_KERNELS_STATS_H

#include <dftracer/utils/dataframe/dataframe.h>

#include <cstdint>

// Summary statistics over a FLAT column, skipping nulls. Moment-based stats
// (variance/stddev/skewness/kurtosis) are numeric-only; quantile/median sort
// the values (SIMD VQSort); unique/nunique work for any type via argsort.
namespace dftracer::utils::dataframe {

/// Variance of the non-null values. `sample` divides by (n-1) (Bessel), else by
/// n. 0 when fewer than 2 (sample) or 1 (population) values.
double variance(const Series& v, bool sample = true);

/// Standard deviation (sqrt of variance).
double stddev(const Series& v, bool sample = true);

/// Population skewness (Fisher-Pearson g1); 0 when undefined.
double skewness(const Series& v);

/// Excess kurtosis (population; normal == 0); 0 when undefined.
double kurtosis(const Series& v);

/// The q-th quantile (q in [0, 1]) with linear interpolation between the two
/// nearest ranks. NaN when there are no non-null values.
double quantile(const Series& v, double q);

/// The median (quantile 0.5).
double median(const Series& v);

/// Number of distinct non-null values.
std::int64_t nunique(const Series& v);

/// The distinct non-null values, in ascending order.
Series unique(const Series& v);

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
Series search_sorted(const Series& v, const Series& values);

/// Linearly interpolate null interior values between their nearest non-null
/// neighbors (by row index); leading and trailing nulls stay null. Float64
/// column.
Series interpolate(const Series& v);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_KERNELS_STATS_H
