#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/common/statistics/log2_histogram.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <string>

using namespace dftracer::utils::utilities::common::statistics;

TEST_SUITE("Log2Histogram") {
    TEST_CASE("bin_index - edge cases") {
        CHECK(Log2Histogram::bin_index(0) == 0);
        CHECK(Log2Histogram::bin_index(1) == 1);
        CHECK(Log2Histogram::bin_index(2) == 2);
        CHECK(Log2Histogram::bin_index(3) == 2);
        CHECK(Log2Histogram::bin_index(4) == 3);
        CHECK(Log2Histogram::bin_index(7) == 3);
        CHECK(Log2Histogram::bin_index(8) == 4);
        CHECK(Log2Histogram::bin_index(UINT64_MAX) == 64);
    }

    TEST_CASE("bin_index - powers of two") {
        for (std::size_t k = 0; k < 63; ++k) {
            std::uint64_t val = static_cast<std::uint64_t>(1) << k;
            CHECK(Log2Histogram::bin_index(val) == k + 1);
        }
    }

    TEST_CASE("bin_lower and bin_upper") {
        CHECK(Log2Histogram::bin_lower(0) == 0);
        CHECK(Log2Histogram::bin_upper(0) == 0);
        CHECK(Log2Histogram::bin_lower(1) == 1);
        CHECK(Log2Histogram::bin_upper(1) == 2);
        CHECK(Log2Histogram::bin_lower(2) == 2);
        CHECK(Log2Histogram::bin_upper(2) == 4);
        CHECK(Log2Histogram::bin_lower(3) == 4);
        CHECK(Log2Histogram::bin_upper(3) == 8);
    }

    TEST_CASE("add and total_count") {
        Log2Histogram h;
        h.add(0);
        h.add(1);
        h.add(100);
        h.add(100);
        CHECK(h.total_count() == 4);
        CHECK(h.bins()[0] == 1);  // value 0
        CHECK(h.bins()[1] == 1);  // value 1
        CHECK(h.bins()[7] == 2);  // values 100 (bin [64, 128))
    }

    TEST_CASE("add with count") {
        Log2Histogram h;
        h.add(1024, 5);
        CHECK(h.total_count() == 5);
        CHECK(h.bins()[11] == 5);  // bin [1024, 2048)
    }

    TEST_CASE("merge commutativity") {
        Log2Histogram a, b;
        a.add(10, 3);
        a.add(1000, 2);
        b.add(10, 1);
        b.add(5000, 4);

        Log2Histogram ab = a;
        ab.merge(b);

        Log2Histogram ba = b;
        ba.merge(a);

        CHECK(ab.total_count() == ba.total_count());
        for (std::size_t i = 0; i < Log2Histogram::NUM_BINS; ++i) {
            CHECK(ab.bins()[i] == ba.bins()[i]);
        }
    }

    TEST_CASE("approx_percentile - basic") {
        Log2Histogram h;
        // All values in bin 1 [1, 2)
        h.add(1, 100);
        double p50 = h.approx_percentile(0.5);
        CHECK(p50 >= 1.0);
        CHECK(p50 <= 2.0);
    }

    TEST_CASE("approx_percentile - empty histogram") {
        Log2Histogram h;
        CHECK(h.approx_percentile(0.5) == 0.0);
    }

    TEST_CASE("approx_percentile - boundary values") {
        Log2Histogram h;
        h.add(1, 100);
        CHECK(h.approx_percentile(0.0) == 0.0);
        double p100 = h.approx_percentile(1.0);
        CHECK(p100 > 0.0);
    }

    TEST_CASE("render_ascii - non-empty") {
        Log2Histogram h;
        h.add(1, 10);
        h.add(4, 20);
        h.add(100, 5);
        std::string result = h.render_ascii(40, "us");
        CHECK(!result.empty());
        CHECK(result.find("us") != std::string::npos);
        CHECK(result.find("#") != std::string::npos);
    }

    TEST_CASE("render_ascii - empty") {
        Log2Histogram h;
        std::string result = h.render_ascii(40, "us");
        CHECK(result.find("no data") != std::string::npos);
    }

    TEST_CASE("to_json / from_json round-trip") {
        Log2Histogram original;
        original.add(0, 3);
        original.add(1, 5);
        original.add(100, 10);
        original.add(1000000, 1);

        std::string json = original.to_json();
        Log2Histogram restored = Log2Histogram::from_json(json);

        CHECK(restored.total_count() == original.total_count());
        for (std::size_t i = 0; i < Log2Histogram::NUM_BINS; ++i) {
            CHECK(restored.bins()[i] == original.bins()[i]);
        }
    }

    TEST_CASE("to_json - sparse representation") {
        Log2Histogram h;
        h.add(42, 1);
        std::string json = h.to_json();
        // Should only contain one entry, not 65
        CHECK(json.find("[[") != std::string::npos);
    }

    TEST_CASE("from_json - empty/invalid") {
        auto h1 = Log2Histogram::from_json("");
        CHECK(h1.total_count() == 0);

        auto h2 = Log2Histogram::from_json("invalid");
        CHECK(h2.total_count() == 0);

        auto h3 = Log2Histogram::from_json("[]");
        CHECK(h3.total_count() == 0);
    }
}
