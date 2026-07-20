#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/time_metric.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::utilities::composites::dft;

TEST_SUITE("TimeMetric") {
    TEST_CASE("parse and to_string round-trip") {
        CHECK(parse_time_metric("NS") == TimeMetric::NS);
        CHECK(parse_time_metric("US") == TimeMetric::US);
        CHECK(parse_time_metric("MS") == TimeMetric::MS);
        CHECK(parse_time_metric("SEC") == TimeMetric::SEC);
        CHECK(parse_time_metric("") == TimeMetric::US);
        CHECK(parse_time_metric("bogus") == TimeMetric::US);
        CHECK(time_metric_to_string(TimeMetric::NS) == "NS");
        CHECK(time_metric_to_string(TimeMetric::SEC) == "SEC");
        CHECK(time_metric_to_string(TimeMetric::US) == "US");
    }

    TEST_CASE("scale_to_us") {
        CHECK(scale_to_us(TimeMetric::US, 5) == 5);
        CHECK(scale_to_us(TimeMetric::NS, 5000000000ULL) == 5000000ULL);
        CHECK(scale_to_us(TimeMetric::MS, 5) == 5000ULL);
        CHECK(scale_to_us(TimeMetric::SEC, 5) == 5000000ULL);
    }

    TEST_CASE("scale_between arbitrary units") {
        CHECK(scale_between(TimeMetric::NS, TimeMetric::US, 5000) == 5);
        CHECK(scale_between(TimeMetric::NS, TimeMetric::MS, 5000000) == 5);
        CHECK(scale_between(TimeMetric::SEC, TimeMetric::NS, 2) ==
              2000000000ULL);
        CHECK(scale_between(TimeMetric::MS, TimeMetric::US, 3) == 3000);
        CHECK(scale_between(TimeMetric::US, TimeMetric::US, 42) == 42);
        // downscale truncates sub-unit precision
        CHECK(scale_between(TimeMetric::NS, TimeMetric::US, 1500) == 1);
    }

    TEST_CASE("extract_time_metric from CM event") {
        DFTracerEvent ev;
        ev.ph = "M";
        ev.name = "CM";
        ev.args.set_valid(true);
        ev.args.insert("name", std::string("time_metric"));
        ev.args.insert("value", std::string("NS"));
        TimeMetric out = TimeMetric::US;
        CHECK(extract_time_metric(ev, out));
        CHECK(out == TimeMetric::NS);
    }

    TEST_CASE("extract_time_metric ignores non-CM events") {
        DFTracerEvent ev;
        ev.ph = "X";
        ev.name = "read";
        TimeMetric out = TimeMetric::US;
        CHECK_FALSE(extract_time_metric(ev, out));
        CHECK(out == TimeMetric::US);
    }
}
