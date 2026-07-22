#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/common/statistics/distinct_sketch.h>
#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace dftracer::utils::utilities::common::statistics;

namespace {
double rel_error(std::uint64_t got, std::uint64_t expected) {
    if (expected == 0) return got == 0 ? 0.0 : 1.0;
    return std::fabs(static_cast<double>(got) - static_cast<double>(expected)) /
           static_cast<double>(expected);
}

void fill(DistinctSketch& s, std::uint64_t begin, std::uint64_t end) {
    for (std::uint64_t i = begin; i < end; ++i)
        s.add("fhash_" + std::to_string(i));
}
}  // namespace

TEST_SUITE("DistinctSketch") {
    TEST_CASE("DistinctSketch - empty") {
        DistinctSketch s;
        CHECK(s.empty());
        CHECK(s.estimate() == 0);
    }

    TEST_CASE("DistinctSketch - exact while sparse") {
        // Small cardinalities keep their hashes, so the count is not an
        // estimate at all - which is where an error would be noticed.
        for (std::uint64_t n : {1u, 10u, 100u, 1000u}) {
            DistinctSketch s;
            fill(s, 0, n);
            CHECK(s.dense_registers().empty());
            CHECK(s.estimate() == n);
        }
    }

    TEST_CASE("DistinctSketch - duplicates do not inflate the count") {
        DistinctSketch s;
        for (int rep = 0; rep < 50; ++rep) fill(s, 0, 100);
        CHECK(s.estimate() == 100);
    }

    TEST_CASE("DistinctSketch - large cardinality within a few percent") {
        for (std::uint64_t n : {10000u, 100000u, 1000000u}) {
            DistinctSketch s;
            fill(s, 0, n);
            CHECK_FALSE(s.dense_registers().empty());
            CHECK(rel_error(s.estimate(), n) < 0.05);
        }
    }

    TEST_CASE("DistinctSketch - merge equals the union") {
        SUBCASE("both sparse") {
            DistinctSketch a, b;
            fill(a, 0, 100);
            fill(b, 50, 150);
            a.merge_from(b);
            CHECK(a.estimate() == 150);
        }
        SUBCASE("both dense") {
            DistinctSketch a, b;
            fill(a, 0, 60000);
            fill(b, 30000, 90000);
            a.merge_from(b);
            CHECK(rel_error(a.estimate(), 90000) < 0.05);
        }
        SUBCASE("sparse into dense and back") {
            DistinctSketch dense, sparse;
            fill(dense, 0, 20000);
            fill(sparse, 20000, 20100);
            DistinctSketch a = dense;
            a.merge_from(sparse);
            DistinctSketch b = sparse;
            b.merge_from(dense);
            CHECK(rel_error(a.estimate(), 20100) < 0.05);
            CHECK(rel_error(b.estimate(), 20100) < 0.05);
        }
        SUBCASE("merging an empty sketch changes nothing") {
            DistinctSketch a, empty;
            fill(a, 0, 300);
            a.merge_from(empty);
            CHECK(a.estimate() == 300);
        }
    }

    TEST_CASE("DistinctSketch - merge is order independent") {
        DistinctSketch a, b, c;
        fill(a, 0, 1000);
        fill(b, 500, 1500);
        fill(c, 1400, 2000);
        DistinctSketch abc = a;
        abc.merge_from(b);
        abc.merge_from(c);
        DistinctSketch cba = c;
        cba.merge_from(b);
        cba.merge_from(a);
        CHECK(abc.estimate() == cba.estimate());
    }

    TEST_CASE("DistinctSketch - precision trades bytes for accuracy") {
        // Each sketch must land within its own error bound. A single data set
        // can favour either one, so this checks the bounds rather than their
        // order; averaged over data sets the higher precision does win.
        constexpr std::uint64_t N = 500000;
        BasicDistinctSketch<11> low;
        BasicDistinctSketch<14> high;
        double low_total = 0, high_total = 0;
        constexpr int TRIALS = 4;
        for (int t = 0; t < TRIALS; ++t) {
            BasicDistinctSketch<11> l;
            BasicDistinctSketch<14> h;
            for (std::uint64_t i = 0; i < N; ++i) {
                auto v =
                    "t" + std::to_string(t) + "_fhash_" + std::to_string(i);
                l.add(v);
                h.add(v);
            }
            low_total += rel_error(l.estimate(), N);
            high_total += rel_error(h.estimate(), N);
            if (t == 0) {
                low = l;
                high = h;
            }
        }
        CHECK(low.dense_registers().size() == 2048);
        CHECK(high.dense_registers().size() == 16384);
        // 3x the standard error (1.04/sqrt(registers)) for each.
        CHECK(rel_error(low.estimate(), N) < 3 * 1.04 / std::sqrt(2048.0));
        CHECK(rel_error(high.estimate(), N) < 3 * 1.04 / std::sqrt(16384.0));
        CHECK(high_total / TRIALS < low_total / TRIALS);
    }

    TEST_CASE("DistinctSketch - state survives a round trip") {
        SUBCASE("sparse") {
            DistinctSketch s;
            fill(s, 0, 200);
            DistinctSketch restored;
            restored.set_sparse_hashes(s.sparse_hashes());
            CHECK(restored.estimate() == s.estimate());
        }
        SUBCASE("dense") {
            DistinctSketch s;
            fill(s, 0, 50000);
            DistinctSketch restored;
            restored.set_dense_registers(s.dense_registers());
            CHECK(restored.estimate() == s.estimate());
        }
    }
}
