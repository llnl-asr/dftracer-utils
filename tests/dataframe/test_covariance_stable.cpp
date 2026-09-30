// corr, covar and the regressions stay accurate when the means are large next
// to the spread, and any partitioning of the rows merges to the one-pass value.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace df = dftracer::utils::dataframe;
using df::AggOp;
using df::AggSpec;
using df::DataFrame;
using df::Series;

namespace {

constexpr std::size_t N = 1000000;
constexpr std::int32_t VC_Y = 0;  // value column indices
constexpr std::int32_t VC_X = 1;
const std::vector<std::string> G{"g"};

struct Ref {
    double cov_pop, cov_samp, corr, slope, intercept, r2;
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

// The corrected two-pass value over the pairs: deviations from the compensated
// means, with the (sum dx)(sum dy) / n term removing the rounding error of the
// means themselves.
Ref two_pass(const std::vector<double>& x, const std::vector<double>& y) {
    const double n = static_cast<double>(x.size());
    const double mx = compensated_sum(x) / n;
    const double my = compensated_sum(y) / n;
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double dx = x[i] - mx, dy = y[i] - my;
        sx += dx;
        sy += dy;
        sxx += dx * dx;
        syy += dy * dy;
        sxy += dx * dy;
    }
    const double cxx = sxx - sx * sx / n;
    const double cyy = syy - sy * sy / n;
    const double cxy = sxy - sx * sy / n;
    Ref r;
    r.cov_pop = cxy / n;
    r.cov_samp = cxy / (n - 1.0);
    r.corr = cxy / std::sqrt(cxx * cyy);
    r.slope = cxy / cxx;
    r.intercept = my - r.slope * mx;
    r.r2 = r.corr * r.corr;
    return r;
}

double rel(double a, double b) { return std::fabs(a - b) / std::fabs(b); }

// z ~ N(0,1); x = mean_x + spread_x z; y = mean_y + 0.5 spread_x z + e.
void pairs(double mean_x, double mean_y, double spread, std::size_t n,
           unsigned seed, std::vector<double>& x, std::vector<double>& y) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> z(0.0, 1.0);
    x.resize(n);
    y.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double a = z(rng);
        x[i] = mean_x + spread * a;
        y[i] = mean_y + 0.5 * spread * a + spread * z(rng);
    }
}

std::vector<AggSpec> all_specs() {
    return {
        AggSpec{AggOp::Corr, VC_Y, "corr", 0.0, VC_X},
        AggSpec{AggOp::CovarPop, VC_Y, "covar_pop", 0.0, VC_X},
        AggSpec{AggOp::CovarSamp, VC_Y, "covar_samp", 0.0, VC_X},
        AggSpec{AggOp::RegrSlope, VC_Y, "regr_slope", 0.0, VC_X},
        AggSpec{AggOp::RegrIntercept, VC_Y, "regr_intercept", 0.0, VC_X},
        AggSpec{AggOp::RegrR2, VC_Y, "regr_r2", 0.0, VC_X},
    };
}

double at(const DataFrame& d, const char* name, std::int64_t row = 0) {
    return d.column(name).data<double>()[row];
}

void check_against(const DataFrame& r, const Ref& ref, std::int64_t row = 0) {
    CHECK(rel(at(r, "corr", row), ref.corr) < 1e-9);
    CHECK(rel(at(r, "covar_pop", row), ref.cov_pop) < 1e-9);
    CHECK(rel(at(r, "covar_samp", row), ref.cov_samp) < 1e-9);
    CHECK(rel(at(r, "regr_slope", row), ref.slope) < 1e-9);
    CHECK(rel(at(r, "regr_intercept", row), ref.intercept) < 1e-9);
    CHECK(rel(at(r, "regr_r2", row), ref.r2) < 1e-9);
}

DataFrame one_group(const std::vector<double>& x,
                    const std::vector<double>& y) {
    const std::int64_t n = static_cast<std::int64_t>(x.size());
    std::vector<std::int64_t> keys(x.size(), 7);
    Series k = Series::flat_i64(keys.data(), n);
    Series sy = Series::flat_f64(y.data(), n);
    Series sx = Series::flat_f64(x.data(), n);
    return df::group_agg({&k}, {&sy, &sx}, all_specs(),
                         std::vector<std::string>{"g"});
}

}  // namespace

TEST_CASE("a large offset keeps the covariance and correlation") {
    std::vector<double> x, y;
    pairs(1e9, 5e8, 1.0, N, 0, x, y);
    const Ref ref = two_pass(x, y);
    DataFrame r = one_group(x, y);
    REQUIRE(r.num_rows() == 1);
    check_against(r, ref);
    // The spec's numbers (seed 0 of this generator, not numpy's): corr is
    // moderate, never the 0 the raw sums gave.
    CHECK(at(r, "corr") > 0.3);
    CHECK(at(r, "corr") < 0.6);
}

TEST_CASE("a tiny spread keeps the covariance and correlation") {
    std::vector<double> x, y;
    pairs(1e3, 1e3, 1e-3, N, 1, x, y);
    check_against(one_group(x, y), two_pass(x, y));
}

TEST_CASE("a very large mean next to a spread of one") {
    std::vector<double> x, y;
    pairs(6e11, 6e11, 1.0, 200000, 2, x, y);
    check_against(one_group(x, y), two_pass(x, y));
}

TEST_CASE("every group keeps its own accuracy at different offsets") {
    std::vector<double> x0, y0, x1, y1;
    pairs(1e9, 5e8, 1.0, 100000, 3, x0, y0);
    pairs(2e3, -4e4, 1e-2, 100000, 4, x1, y1);
    std::vector<std::int64_t> keys;
    std::vector<double> x, y;
    for (std::size_t i = 0; i < x0.size(); ++i) {
        keys.push_back(0);
        x.push_back(x0[i]);
        y.push_back(y0[i]);
        keys.push_back(1);
        x.push_back(x1[i]);
        y.push_back(y1[i]);
    }
    const std::int64_t n = static_cast<std::int64_t>(x.size());
    Series k = Series::flat_i64(keys.data(), n);
    Series sy = Series::flat_f64(y.data(), n);
    Series sx = Series::flat_f64(x.data(), n);
    DataFrame r = df::group_agg({&k}, {&sy, &sx}, all_specs(),
                                std::vector<std::string>{"g"});
    REQUIRE(r.num_rows() == 2);
    const std::int64_t* g = r.column("g").data<std::int64_t>();
    check_against(r, two_pass(x0, y0), g[0] == 0 ? 0 : 1);
    check_against(r, two_pass(x1, y1), g[0] == 0 ? 1 : 0);
}

TEST_CASE("partitions merge to the one-pass value for any split") {
    std::vector<double> x, y;
    pairs(1e9, 5e8, 1.0, 400000, 5, x, y);
    const Ref ref = two_pass(x, y);
    const std::int64_t n = static_cast<std::int64_t>(x.size());
    std::vector<std::int64_t> keys(x.size(), 7);
    Series k = Series::flat_i64(keys.data(), n);
    Series sy = Series::flat_f64(y.data(), n);
    Series sx = Series::flat_f64(x.data(), n);
    const std::vector<const Series*> kc = {&k};
    const std::vector<const Series*> vc = {&sy, &sx};
    const std::int64_t cuts[] = {0, 90000, 90001, 250000, n};
    auto part = [&](int i) {
        auto st = df::agg_new(all_specs());
        df::agg_accumulate(*st, kc, vc, cuts[i], cuts[i + 1]);
        return st;
    };
    // Left to right, then the two halves merged separately and joined.
    auto a = part(0);
    for (int i = 1; i < 4; ++i) df::agg_merge(*a, *part(i));
    check_against(df::agg_finalize(*a, G), ref);

    auto b = part(3), c = part(0);
    df::agg_merge(*b, *part(2));
    df::agg_merge(*c, *part(1));
    df::agg_merge(*b, *c);
    check_against(df::agg_finalize(*b, G), ref);
}

TEST_CASE("a serialized partial reads back to the same values") {
    std::vector<double> x, y;
    pairs(1e9, 5e8, 1.0, 100000, 6, x, y);
    const std::int64_t n = static_cast<std::int64_t>(x.size());
    std::vector<std::int64_t> keys(x.size(), 7);
    Series k = Series::flat_i64(keys.data(), n);
    Series sy = Series::flat_f64(y.data(), n);
    Series sx = Series::flat_f64(x.data(), n);
    const std::vector<const Series*> kc = {&k};
    const std::vector<const Series*> vc = {&sy, &sx};
    auto st = df::agg_new(all_specs());
    df::agg_accumulate(*st, kc, vc);
    auto back = df::agg_deserialize(df::agg_serialize(*st));
    DataFrame a = df::agg_finalize(*st, G);
    DataFrame b = df::agg_finalize(*back, G);
    for (const char* name : {"corr", "covar_pop", "covar_samp", "regr_slope",
                             "regr_intercept", "regr_r2"})
        CHECK(at(a, name) == at(b, name));
}

TEST_CASE("a column with no spread and a single pair read 0") {
    std::vector<double> x(1000, 5e8), y(1000), z;
    pairs(1e9, 5e8, 1.0, 1000, 7, z, y);
    DataFrame r = one_group(x, y);
    CHECK(at(r, "corr") == 0.0);
    CHECK(at(r, "regr_slope") == 0.0);
    CHECK(at(r, "covar_samp") == 0.0);
    DataFrame one = one_group({1.0}, {2.0});
    CHECK(at(one, "corr") == 0.0);
    CHECK(at(one, "covar_samp") == 0.0);
}
