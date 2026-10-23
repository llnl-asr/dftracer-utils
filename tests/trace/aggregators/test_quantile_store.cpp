#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/sketch.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/schemas/dft/agg/quantile_store.h>
#include <dftracer/utils/memory/heap.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>

using dftracer::utils::dataframe::MergeSketch;
using dftracer::utils::dstd::Vector;
using dftracer::utils::index::schemas::dft::agg::QuantileStore;

namespace {

constexpr double ACCURACY = 0.01;
// A sketch quantile is a bucket midpoint, so it is within
// (gamma - 1) / 2 = 0.0101 of any value in its bucket at accuracy 0.01; the
// bound below adds slack for the rank rule. The SIMD key kernel may put a
// value on the neighbouring bucket at a bucket edge, one gamma (0.0202) away
// from a scalar sketch, so the bound against a scalar sketch is 0.021.
// The data spans 1e9, about 1030 of the 2048 bins, so nothing collapses.
constexpr double TRUE_BOUND = 0.011;
constexpr double DIRECT_BOUND = 0.021;
constexpr double QS[] = {0.0, 0.01, 0.25, 0.5, 0.9, 0.99, 1.0};

double exact_rule(Vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    if (q <= 0.0) return v.front();
    if (q >= 1.0) return v.back();
    const double pos = q * static_cast<double>(n - 1);
    const auto lo = static_cast<std::size_t>(pos);
    const double frac = pos - static_cast<double>(lo);
    if (lo + 1 >= n) return v[lo];
    return v[lo] * (1.0 - frac) + v[lo + 1] * frac;
}

double rel(double a, double b) {
    return std::abs(a - b) / std::max(std::abs(b), 1e-300);
}

QuantileStore build(const Vector<double>& v, std::size_t b, std::size_t e) {
    QuantileStore s(ACCURACY);
    for (std::size_t i = b; i < e; ++i) s.add(v[i]);
    return s;
}

void check_sketch_like(const QuantileStore& s, const Vector<double>& all) {
    REQUIRE(s.count() == all.size());
    MergeSketch direct(ACCURACY);
    for (double x : all) direct.add(x);
    Vector<double> sorted = all;
    std::sort(sorted.begin(), sorted.end());
    for (double q : QS) {
        const double got = s.quantile(q);
        CHECK(rel(got, direct.quantile(q)) <= DIRECT_BOUND);
        auto rank = static_cast<std::size_t>(
            std::ceil(q * static_cast<double>(sorted.size())));
        if (rank == 0) rank = 1;
        CHECK(rel(got, sorted[rank - 1]) <= TRUE_BOUND);
    }
}

// Lognormal with sigma 2: a few thousand values span about 1e9.
Vector<double> lognormal(std::size_t n, unsigned seed) {
    std::mt19937_64 rng(seed);
    std::lognormal_distribution<double> d(std::log(1e4), 2.0);
    Vector<double> v;
    for (std::size_t i = 0; i < n; ++i) v.push_back(d(rng));
    return v;
}

// Two modes 7 orders of magnitude apart: 64 and 1e9.
Vector<double> bimodal(std::size_t n, unsigned seed) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> lo(64.0, 3.0);
    std::normal_distribution<double> hi(1e9, 5e7);
    std::bernoulli_distribution pick(0.3);
    Vector<double> v;
    for (std::size_t i = 0; i < n; ++i)
        v.push_back(pick(rng) ? hi(rng) : lo(rng));
    return v;
}

Vector<double> log_uniform(std::size_t n, unsigned seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> d(0.0, std::log(1e9));
    Vector<double> v;
    for (std::size_t i = 0; i < n; ++i) v.push_back(std::exp(d(rng)));
    return v;
}

dftracer::utils::dstd::Span<const double> dstd_span(const Vector<double>& v,
                                                    std::size_t from) {
    return {v.data() + from, v.size() - from};
}

}  // namespace

TEST_CASE("empty store") {
    QuantileStore s;
    CHECK(s.count() == 0);
    CHECK(s.is_exact());
    CHECK(std::isnan(s.quantile(0.5)));
    MergeSketch d;
    CHECK(std::isnan(d.quantile(0.5)));
}

TEST_CASE("exact up to 64 values") {
    for (std::size_t n : {1, 2, 3, 10, 63, 64}) {
        auto v = lognormal(n, 7);
        QuantileStore s = build(v, 0, n);
        CHECK(s.is_exact());
        CHECK(s.count() == n);
        for (double q : QS) CHECK(s.quantile(q) == exact_rule(v, q));
    }
    Vector<double> ramp;
    for (int i = 1; i <= 64; ++i) ramp.push_back(i);
    QuantileStore s = build(ramp, 0, 64);
    CHECK(s.quantile(0.5) == doctest::Approx(32.5));
}

TEST_CASE("65 values or more become a sketch") {
    for (const auto& v : {lognormal(65, 1), lognormal(1000, 2), bimodal(65, 3),
                          bimodal(5000, 4), log_uniform(5000, 5)}) {
        QuantileStore s = build(v, 0, v.size());
        CHECK_FALSE(s.is_exact());
        check_sketch_like(s, v);
    }
}

TEST_CASE("merge of exact stores under the capacity stays exact") {
    auto v = bimodal(60, 5);
    QuantileStore a = build(v, 0, 25);
    QuantileStore b = build(v, 25, 60);
    a.merge(b);
    CHECK(a.is_exact());
    CHECK(a.count() == 60);
    for (double q : QS) CHECK(a.quantile(q) == exact_rule(v, q));

    auto w = lognormal(64, 6);
    QuantileStore c = build(w, 0, 30);
    QuantileStore d = build(w, 30, 64);
    c.merge(d);
    CHECK(c.is_exact());
    for (double q : QS) CHECK(c.quantile(q) == exact_rule(w, q));
}

TEST_CASE("every merge shape matches a sketch of all values") {
    for (int dist = 0; dist < 3; ++dist) {
        const auto v = dist == 0   ? lognormal(4000, 11)
                       : dist == 1 ? bimodal(4000, 12)
                                   : log_uniform(4000, 13);
        SUBCASE("exact + exact over capacity") {
            QuantileStore a = build(v, 0, 40);
            QuantileStore b = build(v, 40, 100);
            a.merge(b);
            CHECK_FALSE(a.is_exact());
            check_sketch_like(a, [&] {
                Vector<double> p(v.begin(), v.begin() + 100);
                return p;
            }());
        }
        SUBCASE("exact + sketch") {
            QuantileStore a = build(v, 0, 30);
            QuantileStore b = build(v, 30, 2000);
            a.merge(b);
            CHECK_FALSE(a.is_exact());
            check_sketch_like(a, [&] {
                Vector<double> p(v.begin(), v.begin() + 2000);
                return p;
            }());
        }
        SUBCASE("sketch + exact") {
            QuantileStore a = build(v, 0, 2000);
            QuantileStore b = build(v, 2000, 2030);
            a.merge(b);
            CHECK_FALSE(a.is_exact());
            check_sketch_like(a, [&] {
                Vector<double> p(v.begin(), v.begin() + 2030);
                return p;
            }());
        }
        SUBCASE("sketch + sketch") {
            QuantileStore a = build(v, 0, 1500);
            QuantileStore b = build(v, 1500, 4000);
            a.merge(b);
            CHECK_FALSE(a.is_exact());
            check_sketch_like(a, v);
        }
    }
}

TEST_CASE("serialize round trip") {
    for (std::size_t n : {0, 1, 64, 65, 3000}) {
        auto v = lognormal(n, 21);
        QuantileStore s = build(v, 0, n);
        Vector<std::uint8_t> buf;
        buf.push_back(0xAB);
        s.serialize_into(buf);
        auto r = QuantileStore::deserialize(buf.data() + 1, buf.size() - 1);
        REQUIRE(r.has_value());
        CHECK(r->is_exact() == s.is_exact());
        CHECK(r->count() == s.count());
        for (double q : QS) {
            const double a = s.quantile(q);
            const double b = r->quantile(q);
            CHECK((a == b || (std::isnan(a) && std::isnan(b))));
        }
    }
    const std::uint8_t bad[] = {9, 0, 0};
    CHECK_FALSE(QuantileStore::deserialize(bad, sizeof(bad)).has_value());
    CHECK_FALSE(QuantileStore::deserialize(bad, 0).has_value());
}

TEST_CASE("add_many matches adding one value at a time") {
    auto v = lognormal(5000, 31);
    v.push_back(std::nan(""));
    v.push_back(0.0);
    for (std::size_t first : {0, 10, 64, 100}) {
        QuantileStore a(ACCURACY);
        for (std::size_t i = 0; i < first; ++i) a.add(v[i]);
        a.add_many(dstd_span(v, first));
        Vector<double> clean;
        for (double x : v)
            if (!std::isnan(x)) clean.push_back(x);
        check_sketch_like(a, clean);
    }
    QuantileStore small(ACCURACY);
    small.add_many(dstd_span(v, 0).first(20));
    CHECK(small.is_exact());
    CHECK(small.count() == 20);
}

TEST_CASE("conversion sizes the sketch once") {
    // Debug mode counts allocations; ctest sets DFTRACER_UTILS_MEMORY_DEBUG=1.
    auto v = lognormal(64, 41);
    QuantileStore s(ACCURACY);
    for (double x : v) s.add(x);
    REQUIRE(s.is_exact());
    // Built before counting, so only the store's own allocations count.
    Vector<double> inside;
    for (int i = 0; i < 5000; ++i)
        inside.push_back(v[static_cast<std::size_t>(i) % 64]);
    const auto before_convert =
        dftracer::utils::memory::debug_allocation_count();
    REQUIRE(before_convert > 0);

    s.add(v[3]);
    REQUIRE_FALSE(s.is_exact());
    const auto after_convert =
        dftracer::utils::memory::debug_allocation_count();
    // The sketch object and its bins.
    CHECK(after_convert - before_convert == 2);

    // Values inside the key range of the 64 exact values reuse the bins.

    for (int i = 0; i < 2500; ++i) s.add(inside[static_cast<std::size_t>(i)]);
    s.add_many(dstd_span(inside, 2500));
    CHECK(dftracer::utils::memory::debug_allocation_count() == after_convert);
    CHECK(s.count() == 64 + 1 + 5000);
}
