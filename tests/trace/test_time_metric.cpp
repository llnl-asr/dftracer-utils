#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/trace/time_metric.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::trace;

TEST_SUITE("TimeMetric") {
    TEST_CASE("parse and to_string round-trip") {
        CHECK(parse_time_metric("NS") == TimeMetric::Ns);
        CHECK(parse_time_metric("US") == TimeMetric::Us);
        CHECK(parse_time_metric("MS") == TimeMetric::Ms);
        CHECK(parse_time_metric("SEC") == TimeMetric::Sec);
        CHECK(parse_time_metric("") == TimeMetric::Us);
        CHECK(parse_time_metric("bogus") == TimeMetric::Us);
        CHECK(time_metric_to_string(TimeMetric::Ns) == "NS");
        CHECK(time_metric_to_string(TimeMetric::Sec) == "SEC");
        CHECK(time_metric_to_string(TimeMetric::Us) == "US");
    }

    TEST_CASE("scale_to_us") {
        CHECK(scale_to_us(TimeMetric::Us, 5) == 5);
        CHECK(scale_to_us(TimeMetric::Ns, 5000000000ULL) == 5000000ULL);
        CHECK(scale_to_us(TimeMetric::Ms, 5) == 5000ULL);
        CHECK(scale_to_us(TimeMetric::Sec, 5) == 5000000ULL);
    }

    TEST_CASE("scale_between arbitrary units") {
        CHECK(scale_between(TimeMetric::Ns, TimeMetric::Us, 5000) == 5);
        CHECK(scale_between(TimeMetric::Ns, TimeMetric::Ms, 5000000) == 5);
        CHECK(scale_between(TimeMetric::Sec, TimeMetric::Ns, 2) ==
              2000000000ULL);
        CHECK(scale_between(TimeMetric::Ms, TimeMetric::Us, 3) == 3000);
        CHECK(scale_between(TimeMetric::Us, TimeMetric::Us, 42) == 42);
        // downscale truncates sub-unit precision
        CHECK(scale_between(TimeMetric::Ns, TimeMetric::Us, 1500) == 1);
    }

    TEST_CASE("extract_time_metric from CM event") {
        DFTracerEvent ev;
        ev.phase = RecordPhase::Metadata;
        ev.name = "CM";
        ev.args.set_valid(true);
        ev.args.insert("name", std::string("time_metric"));
        ev.args.insert("value", std::string("NS"));
        TimeMetric out = TimeMetric::Us;
        CHECK(extract_time_metric(ev, out));
        CHECK(out == TimeMetric::Ns);
    }

    TEST_CASE("extract_time_metric ignores non-CM events") {
        DFTracerEvent ev;
        ev.phase = RecordPhase::Complete;
        ev.name = "read";
        TimeMetric out = TimeMetric::Us;
        CHECK_FALSE(extract_time_metric(ev, out));
        CHECK(out == TimeMetric::Us);
    }
}
