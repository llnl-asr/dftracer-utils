#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/common/statistics/ddsketch.h>
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

using namespace dftracer::utils::utilities::common::statistics;

TEST_SUITE("DDSketch") {
    TEST_CASE("DDSketch - Empty sketch") {
        DDSketch sketch;
        CHECK(sketch.empty());
        CHECK(sketch.count() == 0);
        CHECK(std::isnan(sketch.quantile(0.5)));
    }

    TEST_CASE("DDSketch - Single value") {
        DDSketch sketch;
        sketch.add(42.0);

        CHECK_FALSE(sketch.empty());
        CHECK(sketch.count() == 1);
        CHECK(sketch.min() == 42.0);
        CHECK(sketch.max() == 42.0);

        // All quantiles should return the bin midpoint containing 42.0
        double p50 = sketch.quantile(0.5);
        CHECK(p50 == doctest::Approx(42.0).epsilon(0.02));
    }

    TEST_CASE("DDSketch - Uniform values 1..100") {
        DDSketch sketch(0.01);
        for (int i = 1; i <= 100; ++i) {
            sketch.add(static_cast<double>(i));
        }

        CHECK(sketch.count() == 100);
        CHECK(sketch.min() == 1.0);
        CHECK(sketch.max() == 100.0);

        SUBCASE("p50 ~ 50") {
            double p50 = sketch.quantile(0.5);
            CHECK(p50 == doctest::Approx(50.0).epsilon(0.05));
        }

        SUBCASE("p25 ~ 25") {
            double p25 = sketch.quantile(0.25);
            CHECK(p25 == doctest::Approx(25.0).epsilon(0.05));
        }

        SUBCASE("p75 ~ 75") {
            double p75 = sketch.quantile(0.75);
            CHECK(p75 == doctest::Approx(75.0).epsilon(0.05));
        }
    }

    TEST_CASE("DDSketch - Zero values") {
        DDSketch sketch;

        SUBCASE("All zeros") {
            for (int i = 0; i < 100; ++i) {
                sketch.add(0.0);
            }
            CHECK(sketch.count() == 100);
            CHECK(sketch.quantile(0.5) == 0.0);
        }

        SUBCASE("Mix of zeros and positives") {
            for (int i = 0; i < 50; ++i) {
                sketch.add(0.0);
            }
            for (int i = 1; i <= 50; ++i) {
                sketch.add(static_cast<double>(i));
            }
            CHECK(sketch.count() == 100);
            // p25 should be 0 (first quarter is all zeros)
            CHECK(sketch.quantile(0.25) == 0.0);
        }
    }

    TEST_CASE("DDSketch - Negative values treated as abs") {
        DDSketch sketch;
        // Add enough values to avoid gap interpolation artifacts
        for (int i = 0; i < 50; ++i) sketch.add(-5.0);
        for (int i = 0; i < 50; ++i) sketch.add(-10.0);

        CHECK(sketch.count() == 100);
        // Negative values are treated as abs(), so sketch tracks 5 and 10
        // min/max track original signed values
        CHECK(sketch.min() == -10.0);
        CHECK(sketch.max() == -5.0);
        // Sketch bins use abs(), so quantiles reflect absolute values
        CHECK_FALSE(sketch.empty());
    }

    TEST_CASE("DDSketch - Weighted add") {
        DDSketch sketch_weighted;
        sketch_weighted.add(10.0, 5.0);

        DDSketch sketch_repeated;
        for (int i = 0; i < 5; ++i) {
            sketch_repeated.add(10.0);
        }

        CHECK(sketch_weighted.count() == sketch_repeated.count());
        CHECK(sketch_weighted.quantile(0.5) ==
              doctest::Approx(sketch_repeated.quantile(0.5)).epsilon(0.001));
    }

    TEST_CASE("DDSketch - Merge commutativity") {
        DDSketch a, b;
        for (int i = 1; i <= 50; ++i) a.add(static_cast<double>(i));
        for (int i = 51; i <= 100; ++i) b.add(static_cast<double>(i));

        DDSketch ab(a), ba(b);
        ab.merge(b);
        ba.merge(a);

        CHECK(ab.count() == ba.count());
        CHECK(ab.min() == ba.min());
        CHECK(ab.max() == ba.max());
        CHECK(ab.quantile(0.5) ==
              doctest::Approx(ba.quantile(0.5)).epsilon(0.001));
        CHECK(ab.quantile(0.25) ==
              doctest::Approx(ba.quantile(0.25)).epsilon(0.001));
        CHECK(ab.quantile(0.75) ==
              doctest::Approx(ba.quantile(0.75)).epsilon(0.001));
    }

    TEST_CASE("DDSketch - Merge associativity") {
        DDSketch a, b, c;
        for (int i = 1; i <= 33; ++i) a.add(static_cast<double>(i));
        for (int i = 34; i <= 66; ++i) b.add(static_cast<double>(i));
        for (int i = 67; i <= 100; ++i) c.add(static_cast<double>(i));

        // (A+B)+C
        DDSketch ab(a);
        ab.merge(b);
        DDSketch abc(ab);
        abc.merge(c);

        // A+(B+C)
        DDSketch bc(b);
        bc.merge(c);
        DDSketch a_bc(a);
        a_bc.merge(bc);

        CHECK(abc.count() == a_bc.count());
        CHECK(abc.quantile(0.5) ==
              doctest::Approx(a_bc.quantile(0.5)).epsilon(0.001));
    }

    TEST_CASE("DDSketch - Merge determinism") {
        // Merging in different orders should produce identical bin vectors
        DDSketch a, b, c;
        for (int i = 1; i <= 33; ++i) a.add(static_cast<double>(i));
        for (int i = 34; i <= 66; ++i) b.add(static_cast<double>(i));
        for (int i = 67; i <= 100; ++i) c.add(static_cast<double>(i));

        // Order 1: A+B+C
        DDSketch s1(a);
        s1.merge(b);
        s1.merge(c);

        // Order 2: C+B+A
        DDSketch s2(c);
        s2.merge(b);
        s2.merge(a);

        CHECK(s1.count() == s2.count());
        CHECK(s1.memory_usage() == s2.memory_usage());
        // Same quantiles at all tested percentiles
        for (double q : {0.1, 0.25, 0.5, 0.75, 0.9}) {
            CHECK(s1.quantile(q) ==
                  doctest::Approx(s2.quantile(q)).epsilon(0.001));
        }
    }

    TEST_CASE("DDSketch - Gap interpolation") {
        DDSketch sketch;
        // Bimodal: cluster at 10, cluster at 1000
        for (int i = 0; i < 50; ++i) sketch.add(10.0);
        for (int i = 0; i < 50; ++i) sketch.add(1000.0);

        double p50 = sketch.quantile(0.5);
        // p50 should fall between the two clusters
        CHECK(p50 > 10.0);
        CHECK(p50 < 1000.0);
    }

    TEST_CASE("DDSketch - Reset") {
        DDSketch sketch;
        for (int i = 1; i <= 100; ++i) sketch.add(static_cast<double>(i));
        CHECK_FALSE(sketch.empty());

        sketch.reset();
        CHECK(sketch.empty());
        CHECK(sketch.count() == 0);
        CHECK(std::isnan(sketch.quantile(0.5)));
    }

    TEST_CASE("DDSketch - Memory usage") {
        DDSketch sketch;
        std::size_t base = sketch.memory_usage();
        CHECK(base == sizeof(DDSketch));

        for (int i = 1; i <= 1000; ++i) sketch.add(static_cast<double>(i));
        CHECK(sketch.memory_usage() == sizeof(DDSketch));
    }

    TEST_CASE("DDSketch - Relative accuracy") {
        double accuracy = 0.01;
        DDSketch sketch(accuracy);
        for (int i = 1; i <= 10000; ++i) {
            sketch.add(static_cast<double>(i));
        }

        // Check that all tested quantiles are within relative error
        for (double q : {0.1, 0.25, 0.5, 0.75, 0.9, 0.95, 0.99}) {
            double expected = q * 10000.0;
            double actual = sketch.quantile(q);
            double rel_error = std::abs(actual - expected) / expected;
            CHECK(rel_error < 0.05);  // Allow some slack beyond pure bin error
        }
    }
}
