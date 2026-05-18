#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/common/statistics/timestamp_histogram.h>
#include <doctest/doctest.h>

#include <cstdint>

using namespace dftracer::utils::utilities::common::statistics;

TEST_SUITE("TimestampHistogram") {
    TEST_CASE("empty histogram") {
        TimestampHistogram h;
        CHECK(h.empty());
        CHECK(h.total_count() == 0);
        CHECK(h.num_bins() == 0);
        CHECK(h.count_in_range(0, 1'000'000) == 0);
        CHECK(h.selectivity(0, 1'000'000) == 0.0);
    }

    TEST_CASE("single event") {
        TimestampHistogram h;
        h.add(500'000);  // 500ms -> bin 5

        CHECK(h.total_count() == 1);
        CHECK(h.num_bins() == 1);
        CHECK(h.bins()[0].first == 5);
        CHECK(h.bins()[0].second == 1);
    }

    TEST_CASE("bin_index static") {
        CHECK(TimestampHistogram::bin_index(0) == 0);
        CHECK(TimestampHistogram::bin_index(99'999) == 0);
        CHECK(TimestampHistogram::bin_index(100'000) == 1);
        CHECK(TimestampHistogram::bin_index(199'999) == 1);
        CHECK(TimestampHistogram::bin_index(200'000) == 2);
        CHECK(TimestampHistogram::bin_index(1'000'000) == 10);
    }

    TEST_CASE("bin_start_us and bin_end_us") {
        CHECK(TimestampHistogram::bin_start_us(0) == 0);
        CHECK(TimestampHistogram::bin_end_us(0) == 100'000);
        CHECK(TimestampHistogram::bin_start_us(10) == 1'000'000);
        CHECK(TimestampHistogram::bin_end_us(10) == 1'100'000);
    }

    TEST_CASE("multiple events in same bin") {
        TimestampHistogram h;
        h.add(150'000);
        h.add(160'000);
        h.add(190'000);

        CHECK(h.total_count() == 3);
        CHECK(h.num_bins() == 1);
        CHECK(h.bins()[0].first == 1);
        CHECK(h.bins()[0].second == 3);
    }

    TEST_CASE("events across bins") {
        TimestampHistogram h;
        h.add(50'000);     // bin 0
        h.add(150'000);    // bin 1
        h.add(250'000);    // bin 2
        h.add(1'050'000);  // bin 10

        CHECK(h.total_count() == 4);
        CHECK(h.num_bins() == 4);
        CHECK(h.bins()[0] ==
              std::make_pair(std::uint64_t{0}, std::uint64_t{1}));
        CHECK(h.bins()[1] ==
              std::make_pair(std::uint64_t{1}, std::uint64_t{1}));
        CHECK(h.bins()[2] ==
              std::make_pair(std::uint64_t{2}, std::uint64_t{1}));
        CHECK(h.bins()[3] ==
              std::make_pair(std::uint64_t{10}, std::uint64_t{1}));
    }

    TEST_CASE("count_in_range") {
        TimestampHistogram h;
        // 10 events at 0.0-0.1s, 20 at 0.5-0.6s, 5 at 1.0-1.1s
        for (int i = 0; i < 10; ++i) h.add(50'000);
        for (int i = 0; i < 20; ++i) h.add(550'000);
        for (int i = 0; i < 5; ++i) h.add(1'050'000);

        CHECK(h.count_in_range(0, 100'000) == 10);
        CHECK(h.count_in_range(0, 600'000) == 30);
        CHECK(h.count_in_range(0, 2'000'000) == 35);
        CHECK(h.count_in_range(500'000, 600'000) == 20);
        CHECK(h.count_in_range(500'000, 1'100'000) == 25);
        CHECK(h.count_in_range(200'000, 400'000) == 0);
    }

    TEST_CASE("selectivity") {
        TimestampHistogram h;
        for (int i = 0; i < 100; ++i) h.add(50'000);
        for (int i = 0; i < 100; ++i) h.add(550'000);

        CHECK(h.selectivity(0, 100'000) == doctest::Approx(0.5));
        CHECK(h.selectivity(500'000, 600'000) == doctest::Approx(0.5));
        CHECK(h.selectivity(0, 600'000) == doctest::Approx(1.0));
        CHECK(h.selectivity(200'000, 400'000) == doctest::Approx(0.0));
    }

    TEST_CASE("merge") {
        TimestampHistogram a;
        a.add(50'000);   // bin 0
        a.add(150'000);  // bin 1

        TimestampHistogram b;
        b.add(50'000);   // bin 0
        b.add(250'000);  // bin 2

        a.merge(b);

        CHECK(a.total_count() == 4);
        CHECK(a.num_bins() == 3);
        CHECK(a.bins()[0] ==
              std::make_pair(std::uint64_t{0}, std::uint64_t{2}));
        CHECK(a.bins()[1] ==
              std::make_pair(std::uint64_t{1}, std::uint64_t{1}));
        CHECK(a.bins()[2] ==
              std::make_pair(std::uint64_t{2}, std::uint64_t{1}));
    }

    TEST_CASE("merge with empty") {
        TimestampHistogram a;
        a.add(50'000);

        TimestampHistogram empty;
        a.merge(empty);

        CHECK(a.total_count() == 1);
        CHECK(a.num_bins() == 1);
    }

    TEST_CASE("expansion_weights - uniform") {
        TimestampHistogram h;
        for (int i = 0; i < 100; ++i) h.add(i * 10'000);  // 0-1s uniform

        auto weights = h.expansion_weights(0, 1'000'000, 10);
        CHECK(weights.size() == 10);
        for (auto w : weights) {
            CHECK(w == doctest::Approx(0.1).epsilon(0.01));
        }
    }

    TEST_CASE("expansion_weights - bursty") {
        TimestampHistogram h;
        // 800 events in 0.2-0.4s, 200 events elsewhere
        for (int i = 0; i < 100; ++i) h.add(50'000);   // bin 0
        for (int i = 0; i < 400; ++i) h.add(250'000);  // bin 2
        for (int i = 0; i < 400; ++i) h.add(350'000);  // bin 3
        for (int i = 0; i < 100; ++i) h.add(950'000);  // bin 9

        auto weights = h.expansion_weights(0, 1'000'000, 5);
        CHECK(weights.size() == 5);
        // sub 0 [0-200ms]: bin 0 = 100
        // sub 1 [200-400ms]: bins 2+3 = 800
        // sub 2 [400-600ms]: 0
        // sub 3 [600-800ms]: 0
        // sub 4 [800-1000ms]: bin 9 = 100
        CHECK(weights[0] == doctest::Approx(0.1).epsilon(0.01));
        CHECK(weights[1] == doctest::Approx(0.8).epsilon(0.01));
        CHECK(weights[2] == doctest::Approx(0.0));
        CHECK(weights[3] == doctest::Approx(0.0));
        CHECK(weights[4] == doctest::Approx(0.1).epsilon(0.01));
    }

    TEST_CASE("expansion_weights - no data in range falls back to uniform") {
        TimestampHistogram h;
        h.add(5'000'000);  // 5s, outside query range

        auto weights = h.expansion_weights(0, 1'000'000, 5);
        CHECK(weights.size() == 5);
        for (auto w : weights) {
            CHECK(w == doctest::Approx(0.2));
        }
    }

    TEST_CASE("serialize and deserialize roundtrip") {
        TimestampHistogram h;
        h.add(50'000);
        h.add(150'000);
        h.add(150'000);
        h.add(1'000'050'000);

        auto data = h.serialize();
        auto h2 = TimestampHistogram::deserialize(data.data(), data.size());

        CHECK(h2.total_count() == h.total_count());
        CHECK(h2.num_bins() == h.num_bins());
        REQUIRE(h2.bins().size() == h.bins().size());
        for (std::size_t i = 0; i < h.bins().size(); ++i) {
            CHECK(h2.bins()[i].first == h.bins()[i].first);
            CHECK(h2.bins()[i].second == h.bins()[i].second);
        }
    }

    TEST_CASE("serialize empty") {
        TimestampHistogram h;
        auto data = h.serialize();
        auto h2 = TimestampHistogram::deserialize(data.data(), data.size());
        CHECK(h2.empty());
        CHECK(h2.total_count() == 0);
    }

    TEST_CASE("deserialize null/empty") {
        auto h = TimestampHistogram::deserialize(nullptr, 0);
        CHECK(h.empty());
    }

    TEST_CASE("varint encoding handles large timestamps") {
        TimestampHistogram h;
        // Typical 2026 timestamp: ~1.77e15 us
        h.add(1'773'074'570'000'000ULL);
        h.add(1'773'074'570'100'000ULL);

        auto data = h.serialize();
        auto h2 = TimestampHistogram::deserialize(data.data(), data.size());

        CHECK(h2.total_count() == 2);
        CHECK(h2.num_bins() == 2);
        CHECK(h2.bins()[0].first == h.bins()[0].first);
        CHECK(h2.bins()[1].first == h.bins()[1].first);
    }

    TEST_CASE("serialization is compact with delta encoding") {
        TimestampHistogram h;
        // 100 consecutive bins (10s of data)
        std::uint64_t base = 17'730'745'700ULL;  // ~2026 timestamp / 100ms
        for (std::uint64_t i = 0; i < 100; ++i) {
            for (int j = 0; j < 50; ++j) {
                h.add((base + i) * 100'000 + j * 1000);
            }
        }

        auto data = h.serialize();
        // 100 bins with delta=1 each = ~1 byte per delta + ~1 byte per count
        // Plus header. Should be well under 500 bytes.
        CHECK(data.size() < 500);
    }
}
