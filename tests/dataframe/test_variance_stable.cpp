// var/std/skew/kurt stay accurate when the mean is large next to the spread,
// and any partitioning of a column merges to the one-pass value.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/agg_expr.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/field_stat.h>
#include <dftracer/utils/dataframe/kernels/field_stat.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <random>
#include <vector>

using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::FieldStat;
using dftracer::utils::dataframe::Series;

namespace df = dftracer::utils::dataframe;

namespace {

struct Ref {
    double var, sd, skew, kurt;
};

// Neumaier-compensated sum: the reference must not lose digits to the very
// cancellation under test (on some targets `long double` is a plain double).
double compensated_sum(const std::vector<double>& v) {
    double sum = 0.0, c = 0.0;
    for (double x : v) {
        const double t = sum + x;
        c += std::fabs(sum) >= std::fabs(x) ? (sum - t) + x : (x - t) + sum;
        sum = t;
    }
    return sum + c;
}

// The corrected two-pass value: deviations from the compensated mean, with the
// (sum d)^2 / n term removing the rounding error of the mean itself.
Ref two_pass(const std::vector<double>& v) {
    const double n = static_cast<double>(v.size());
    const double mu = compensated_sum(v) / n;
    double s1 = 0, s2 = 0, s3 = 0, s4 = 0;
    for (double x : v) {
        const double d = x - mu, dd = d * d;
        s1 += d;
        s2 += dd;
        s3 += dd * d;
        s4 += dd * dd;
    }
    const double e = s1 / n;
    const double m2 = s2 - s1 * e;
    const double m3 = s3 - 3.0 * e * s2 + 2.0 * n * e * e * e;
    const double m4 =
        s4 - 4.0 * e * s3 + 6.0 * e * e * s2 - 3.0 * n * e * e * e * e;
    const double var = m2 / (n - 1.0);
    const double c2 = m2 / n;
    return {var, std::sqrt(var), (m3 / n) / std::pow(c2, 1.5),
            (m4 / n) / (c2 * c2) - 3.0};
}

// Exact for integers: deviations from an integer near the mean, summed in
// 128-bit integers, so no rounding enters before the final division.
__extension__ typedef __int128 Wide;

double exact_std(const std::vector<std::int64_t>& v) {
    Wide s = 0;
    for (auto x : v) s += x;
    const std::int64_t k =
        static_cast<std::int64_t>(s / static_cast<Wide>(v.size()));
    Wide d1 = 0, d2 = 0;
    for (auto x : v) {
        const Wide d = static_cast<Wide>(x) - k;
        d1 += d;
        d2 += d * d;
    }
    const long double n = static_cast<long double>(v.size());
    const long double m2 =
        static_cast<long double>(d2) -
        static_cast<long double>(d1) * static_cast<long double>(d1) / n;
    return static_cast<double>(std::sqrt(m2 / (n - 1.0L)));
}

std::vector<double> normal(double mean, double sd, std::size_t n,
                           std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> z(0.0, 1.0);
    std::vector<double> v(n);
    for (auto& x : v) x = mean + sd * z(rng);
    return v;
}

double rel(double got, double want) {
    return std::fabs(got - want) / std::fabs(want);
}

// Relative to the value, or absolute below 1 (skew and kurt of normal data sit
// near 0, where a relative bound means nothing).
bool close(double got, double want, double tol = 1e-9) {
    return std::fabs(got - want) <= tol * std::max(1.0, std::fabs(want));
}

constexpr std::size_t N = 1'000'000;

}  // namespace

TEST_CASE("a large offset keeps the standard deviation") {
    const auto v = normal(1e9, 1.0, N, 0);
    const Ref ref = two_pass(v);
    Series s = Series::flat_f64(v.data(), static_cast<std::int64_t>(N));
    CHECK(rel(s.stddev(), ref.sd) < 1e-9);
    CHECK(rel(s.variance(), ref.var) < 1e-9);
    FieldStat simd = df::field_stat_reduce(s);
    CHECK(rel(simd.stddev(), ref.sd) < 1e-9);
    FieldStat scalar;  // the per-value path of a nullable column or a group
    for (double x : v) scalar.add(x);
    CHECK(rel(scalar.stddev(), ref.sd) < 1e-9);
}

TEST_CASE("a tiny spread keeps the standard deviation") {
    const auto v = normal(1e3, 1e-3, N, 1);
    const Ref ref = two_pass(v);
    Series s = Series::flat_f64(v.data(), static_cast<std::int64_t>(N));
    CHECK(rel(s.stddev(), ref.sd) < 1e-9);
    FieldStat scalar;
    for (double x : v) scalar.add(x);
    CHECK(rel(scalar.stddev(), ref.sd) < 1e-9);
}

TEST_CASE("an integer column with a large offset") {
    std::mt19937_64 rng(2);
    std::normal_distribution<double> z(0.0, 1000.0);
    std::vector<std::int64_t> iv(N);
    for (std::size_t i = 0; i < N; ++i) {
        iv[i] = 1'700'000'000'000'000LL + static_cast<std::int64_t>(z(rng));
    }
    const double ref_sd = exact_std(iv);
    Series s = Series::flat_i64(iv.data(), static_cast<std::int64_t>(N));
    FieldStat simd = df::field_stat_reduce(s);
    CHECK(rel(simd.stddev(), ref_sd) < 1e-9);
    FieldStat scalar;
    for (std::int64_t x : iv) scalar.add(x);
    CHECK(rel(scalar.stddev(), ref_sd) < 1e-9);
}

TEST_CASE("skewness and kurtosis keep their digits with an offset") {
    const auto v = normal(1e9, 1.0, N, 3);
    const Ref ref = two_pass(v);
    Series s = Series::flat_f64(v.data(), static_cast<std::int64_t>(N));
    FieldStat simd = df::field_stat_reduce(s);
    CHECK_MESSAGE(close(simd.skewness(), ref.skew), simd.skewness()
                                                        << " vs " << ref.skew);
    CHECK_MESSAGE(close(simd.kurtosis(), ref.kurt), simd.kurtosis()
                                                        << " vs " << ref.kurt);
    FieldStat scalar;
    for (double x : v) scalar.add(x);
    CHECK_MESSAGE(close(scalar.skewness(), ref.skew),
                  scalar.skewness() << " vs " << ref.skew);
    CHECK_MESSAGE(close(scalar.kurtosis(), ref.kurt),
                  scalar.kurtosis() << " vs " << ref.kurt);
}

TEST_CASE("a constant column has a standard deviation of 0") {
    std::vector<double> big(N, 1e9);
    Series sb = Series::flat_f64(big.data(), static_cast<std::int64_t>(N));
    CHECK(sb.stddev() == 0.0);
    std::vector<double> small{3.0, 3.0, 3.0, 3.0};
    Series ss = Series::flat_f64(small.data(), 4);
    CHECK(ss.stddev() == 0.0);
    FieldStat f;
    for (double x : big) f.add(x);
    CHECK(f.stddev() == 0.0);
}

TEST_CASE("nulls are skipped") {
    std::vector<double> xs{1.0, 0.0, 3.0};
    std::vector<std::uint8_t> valid{0x05};  // rows 0 and 2
    Series s = Series::flat_f64(xs.data(), 3, valid.data());
    CHECK(s.stddev() == doctest::Approx(std::sqrt(2.0)).epsilon(1e-12));
    CHECK(s.variance() == doctest::Approx(2.0).epsilon(1e-12));
}

TEST_CASE("a state of totals only merges as a state of equal values") {
    FieldStat totals;  // a pre-aggregated row: n and sum, no moments
    totals.n = 3;
    totals.sum = 30.0;
    FieldStat more;
    more.add(10.0);
    more.add(10.0);
    totals.merge(more);
    CHECK(totals.n == 5);
    CHECK(totals.variance() == 0.0);
    FieldStat other;
    other.add(20.0);
    totals.merge(other);  // spread between 10 (x5) and 20
    FieldStat all;
    for (double x : {10.0, 10.0, 10.0, 10.0, 10.0, 20.0}) all.add(x);
    CHECK(totals.variance() == doctest::Approx(all.variance()).epsilon(1e-12));
}

TEST_CASE("partitions merge to the one-pass value for any split") {
    const auto v = normal(1e9, 1.0, N, 4);
    const Ref ref = two_pass(v);
    Series s = Series::flat_f64(v.data(), static_cast<std::int64_t>(N));
    // Eight uneven partitions, one of a single value, and an empty one.
    const std::vector<std::int64_t> cuts{
        0,       1,       1,
        200'000, 333'333, 333'334,
        700'001, 900'000, static_cast<std::int64_t>(N)};
    std::vector<FieldStat> parts;
    for (std::size_t i = 0; i + 1 < cuts.size(); ++i)
        parts.push_back(cuts[i] == cuts[i + 1]
                            ? FieldStat{}
                            : df::field_stat_reduce(s, cuts[i], cuts[i + 1]));
    FieldStat left_to_right;
    for (const auto& p : parts) left_to_right.merge(p);
    CHECK(left_to_right.n == N);
    CHECK(rel(left_to_right.stddev(), ref.sd) < 1e-9);
    CHECK_MESSAGE(close(left_to_right.skewness(), ref.skew),
                  left_to_right.skewness() << " vs " << ref.skew);
    CHECK_MESSAGE(close(left_to_right.kurtosis(), ref.kurt),
                  left_to_right.kurtosis() << " vs " << ref.kurt);
    // A different merge order: a pairwise tree, then right to left.
    std::vector<FieldStat> level = parts;
    while (level.size() > 1) {
        std::vector<FieldStat> next;
        for (std::size_t i = 0; i < level.size(); i += 2) {
            FieldStat a = level[i];
            if (i + 1 < level.size()) a.merge(level[i + 1]);
            next.push_back(a);
        }
        level = std::move(next);
    }
    CHECK(rel(level[0].stddev(), ref.sd) < 1e-9);
    FieldStat right_to_left;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it)
        right_to_left.merge(*it);
    CHECK(rel(right_to_left.stddev(), ref.sd) < 1e-9);
}

TEST_CASE("every group keeps its own accuracy at different offsets") {
    constexpr std::size_t G = 1000, PER = 1000;
    std::mt19937_64 rng(5);
    std::normal_distribution<double> z(0.0, 1.0);
    std::vector<std::int64_t> keys(G * PER);
    std::vector<double> vals(G * PER);
    std::map<std::int64_t, std::vector<double>> by_group;
    for (std::size_t g = 0; g < G; ++g) {
        const double mean = static_cast<double>(g) * 1e9;  // 0 to 1e12
        for (std::size_t j = 0; j < PER; ++j) {
            const std::size_t i = g * PER + j;
            keys[i] = static_cast<std::int64_t>(g);
            vals[i] = mean + z(rng);
            by_group[keys[i]].push_back(vals[i]);
        }
    }
    Series k =
        Series::flat_i64(keys.data(), static_cast<std::int64_t>(keys.size()));
    Series x =
        Series::flat_f64(vals.data(), static_cast<std::int64_t>(vals.size()));
    std::vector<const Series*> inputs = {&k, &x};
    DataFrame r = df::group_agg_expr(
        df::expr_col(0),
        {df::agg_std(df::expr_col(1), "s"), df::agg_var(df::expr_col(1), "v")},
        inputs, "g");
    REQUIRE(r.num_rows() == static_cast<std::int64_t>(G));
    const std::int64_t* gk = r.column("g").data<std::int64_t>();
    const double* s = r.column("s").data<double>();
    const double* v = r.column("v").data<double>();
    std::size_t worst_group = 0;
    double worst = 0;
    for (std::size_t i = 0; i < G; ++i) {
        const Ref ref = two_pass(by_group[gk[i]]);
        const double e = std::max(rel(s[i], ref.sd), rel(v[i], ref.var));
        if (e > worst) {
            worst = e;
            worst_group = static_cast<std::size_t>(gk[i]);
        }
    }
    INFO("worst group " << worst_group);
    CHECK(worst < 1e-9);
}

TEST_CASE("a group-by over one group with a large offset") {
    const auto v = normal(1e9, 1.0, N, 0);
    const Ref ref = two_pass(v);
    std::vector<std::int64_t> keys(N, 7);
    Series k = Series::flat_i64(keys.data(), static_cast<std::int64_t>(N));
    Series x = Series::flat_f64(v.data(), static_cast<std::int64_t>(N));
    std::vector<const Series*> inputs = {&k, &x};
    DataFrame r = df::group_agg_expr(
        df::expr_col(0), {df::agg_std(df::expr_col(1), "s")}, inputs, "g");
    REQUIRE(r.num_rows() == 1);
    CHECK(rel(r.column("s").data<double>()[0], ref.sd) < 1e-9);
}

TEST_CASE("rolling standard deviation stays accurate with an offset") {
    const auto v = normal(1e9, 1.0, 2000, 6);
    Series s = Series::flat_f64(v.data(), static_cast<std::int64_t>(v.size()));
    Series r = s.rolling_std(5);
    const double* out = r.data<double>();
    for (std::size_t i = 4; i < v.size(); ++i) {
        const std::vector<double> w(v.begin() + static_cast<long>(i - 4),
                                    v.begin() + static_cast<long>(i + 1));
        REQUIRE(rel(out[i], two_pass(w).sd) < 1e-9);
    }
}

TEST_CASE("an exponentially weighted standard deviation is shift invariant") {
    // Integer-valued data, so adding 1e9 is exact in double and the shifted
    // column is a true translation of the base column.
    std::mt19937_64 rng(7);
    std::normal_distribution<double> z(500.0, 100.0);
    std::vector<double> base(2000), shifted(2000);
    for (std::size_t i = 0; i < base.size(); ++i) {
        base[i] = std::floor(z(rng));
        shifted[i] = base[i] + 1e9;
    }
    Series a = Series::flat_f64(base.data(), 2000);
    Series b = Series::flat_f64(shifted.data(), 2000);
    Series ea = a.ewm_std(0.3), eb = b.ewm_std(0.3);
    const double* pa = ea.data<double>();
    const double* pb = eb.data<double>();
    for (std::size_t i = 2; i < base.size(); ++i)
        REQUIRE(rel(pb[i], pa[i]) < 1e-9);
}
