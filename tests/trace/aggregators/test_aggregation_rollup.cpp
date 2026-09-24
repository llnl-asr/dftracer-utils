#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/schemas/dft/agg/aggregation_metrics.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

using namespace dftracer::utils::index::schemas::dft::agg;

namespace {

struct Event {
    std::string fhash;
    std::uint64_t dur;
    std::uint64_t sz;
    std::uint64_t off;
    std::uint64_t ts;
};

// Values span distinct magnitudes with no symmetry, so a wrong merge cannot
// coincidentally match on the power sums.
std::vector<Event> sample_events() {
    std::vector<Event> out;
    const char* files[] = {"aa", "bb", "cc", "dd", "ee"};
    std::uint64_t seed = 7;
    for (int i = 0; i < 200; ++i) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        const auto pick = (seed >> 33) % 5;
        out.push_back(Event{files[pick], 100 + (seed >> 17) % 9000,
                            1 + (seed >> 21) % 65536, (seed >> 11) % 1048576,
                            1000 + static_cast<std::uint64_t>(i) * 37});
    }
    return out;
}

void apply(AggregationMetrics& m, const Event& e, bool percentiles) {
    m.update_duration(e.dur, percentiles);
    m.update_size(e.sz, percentiles);
    m.update_offset(e.off, percentiles);
    m.update_timestamp(e.ts, e.dur);
    m.update_custom_metric("retries", e.sz % 7, percentiles);
}

void check_field_equal(const MetricStats& a, const MetricStats& b,
                       const char* what) {
    INFO("field: ", what);
    CHECK(a.count() == b.count());
    CHECK(a.min() == b.min());
    CHECK(a.max() == b.max());
    CHECK(a.total() == doctest::Approx(b.total()));
    CHECK(a.mean() == doctest::Approx(b.mean()));
    CHECK(a.get_stddev() == doctest::Approx(b.get_stddev()));
    CHECK(a.get_skewness() == doctest::Approx(b.get_skewness()));
    CHECK(a.get_kurtosis() == doctest::Approx(b.get_kurtosis()));
}

void check_equivalent(const AggregationMetrics& rolled,
                      const AggregationMetrics& direct) {
    CHECK(rolled.count == direct.count);
    CHECK(rolled.ts == direct.ts);
    CHECK(rolled.te == direct.te);
    check_field_equal(rolled.duration, direct.duration, "duration");
    check_field_equal(rolled.size, direct.size, "size");
    check_field_equal(rolled.offset, direct.offset, "offset");

    REQUIRE(static_cast<bool>(rolled.custom_metrics) ==
            static_cast<bool>(direct.custom_metrics));
    if (rolled.custom_metrics) {
        REQUIRE(rolled.custom_metrics->size() == direct.custom_metrics->size());
        for (const auto& [name, stat] : *rolled.custom_metrics) {
            auto it = direct.custom_metrics->find(name);
            REQUIRE(it != direct.custom_metrics->end());
            check_field_equal(stat, it->second, name.c_str());
        }
    }
}

}  // namespace

// Merging groups is lossless only because FieldStat keeps raw power sums and
// derives the moments at read time. Switching it to an incremental (Welford)
// form would break these.
TEST_SUITE("AggregationRollup") {
    TEST_CASE("rollup of per-file groups equals a direct fileless fold") {
        const bool percentiles = false;
        const auto events = sample_events();

        std::map<std::string, AggregationMetrics> per_file;
        AggregationMetrics direct;
        for (const auto& e : events) {
            auto it = per_file.try_emplace(e.fhash).first;
            apply(it->second, e, percentiles);
            apply(direct, e, percentiles);
        }
        REQUIRE(per_file.size() == 5);

        // Guard against a degenerate sample: if the values were constant or
        // all landed in one file, every check below would pass vacuously.
        REQUIRE(direct.count == events.size());
        REQUIRE(direct.duration.get_stddev() > 1.0);
        REQUIRE(direct.duration.get_skewness() != 0.0);
        REQUIRE(direct.duration.get_kurtosis() != 0.0);
        for (const auto& [_, m] : per_file) REQUIRE(m.count < direct.count);

        AggregationMetrics rolled;
        for (const auto& [_, m] : per_file) rolled.merge_from(m);

        check_equivalent(rolled, direct);
    }

    TEST_CASE("rollup is order-independent across files") {
        const auto events = sample_events();
        std::map<std::string, AggregationMetrics> per_file;
        for (const auto& e : events)
            apply(per_file.try_emplace(e.fhash).first->second, e, false);

        AggregationMetrics forward, reverse;
        for (auto it = per_file.begin(); it != per_file.end(); ++it)
            forward.merge_from(it->second);
        for (auto it = per_file.rbegin(); it != per_file.rend(); ++it)
            reverse.merge_from(it->second);

        CHECK(forward.count == reverse.count);
        CHECK(forward.duration.min() == reverse.duration.min());
        CHECK(forward.duration.max() == reverse.duration.max());
        CHECK(forward.duration.total() ==
              doctest::Approx(reverse.duration.total()));
        CHECK(forward.duration.get_stddev() ==
              doctest::Approx(reverse.duration.get_stddev()));
    }

    TEST_CASE("percentile sketches survive the rollup") {
        const bool percentiles = true;
        const auto events = sample_events();

        std::map<std::string, AggregationMetrics> per_file;
        AggregationMetrics direct;
        for (const auto& e : events) {
            apply(per_file.try_emplace(e.fhash).first->second, e, percentiles);
            apply(direct, e, percentiles);
        }

        AggregationMetrics rolled;
        for (const auto& [_, m] : per_file) rolled.merge_from(m);

        REQUIRE(rolled.duration.sketch);
        REQUIRE(direct.duration.sketch);
        for (double q : {0.5, 0.9, 0.99}) {
            INFO("quantile: ", q);
            CHECK(rolled.duration.sketch->quantile(q) ==
                  doctest::Approx(direct.duration.sketch->quantile(q)));
        }
    }

    TEST_CASE("single-file rollup is the identity") {
        AggregationMetrics only, direct;
        for (const auto& e : sample_events()) {
            if (e.fhash != "aa") continue;
            apply(only, e, false);
            apply(direct, e, false);
        }
        AggregationMetrics rolled;
        rolled.merge_from(only);
        check_equivalent(rolled, direct);
    }
}
