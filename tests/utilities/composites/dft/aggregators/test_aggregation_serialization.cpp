#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::utilities::composites::dft::aggregators;

TEST_SUITE("AggregationSerialization") {
    TEST_CASE("key roundtrip - basic") {
        auto& intern = aggregation_intern();
        AggregationKey key;
        key.cat_id = intern.get_or_insert("POSIX");
        key.name_id = intern.get_or_insert("read");
        key.pid = 12345;
        key.tid = 67890;
        key.hhash_id = intern.get_or_insert("abc123");
        key.fhash_id = intern.get_or_insert("def456");
        key.time_bucket = 5000000;

        auto data = serialize_agg_key(42, AggMapType::EVENT, key);
        auto result = deserialize_agg_key(data);

        CHECK(result.map_type == AggMapType::EVENT);
        CHECK(result.key.cat() == "POSIX");
        CHECK(result.key.name() == "read");
        CHECK(result.key.pid == key.pid);
        CHECK(result.key.tid == key.tid);
        CHECK(result.key.hhash() == "abc123");
        CHECK(result.key.fhash() == "def456");
        CHECK(result.key.time_bucket == key.time_bucket);
        CHECK(result.key.extra_keys == nullptr);
    }

    TEST_CASE("key roundtrip - with extra keys") {
        auto& intern = aggregation_intern();
        AggregationKey key;
        key.cat_id = intern.get_or_insert("MPI");
        key.name_id = intern.get_or_insert("send");
        key.pid = 100;
        key.tid = 200;
        key.time_bucket = 1000000;
        key.extra_keys = std::make_unique<
            std::vector<std::pair<std::uint32_t, std::uint32_t>>>();
        auto ek_a = intern.get_or_insert("epoch");
        auto ev_a = intern.get_or_insert("1");
        auto ek_b = intern.get_or_insert("step");
        auto ev_b = intern.get_or_insert("42");
        key.extra_keys->emplace_back(ek_a, ev_a);
        key.extra_keys->emplace_back(ek_b, ev_b);

        auto data = serialize_agg_key(99, AggMapType::PROFILE, key);
        auto result = deserialize_agg_key(data);

        CHECK(result.map_type == AggMapType::PROFILE);
        CHECK(result.key.cat() == "MPI");
        REQUIRE(result.key.extra_keys != nullptr);
        REQUIRE(result.key.extra_keys->size() == 2);
        CHECK(intern.resolve((*result.key.extra_keys)[0].first) == "epoch");
        CHECK(intern.resolve((*result.key.extra_keys)[0].second) == "1");
        CHECK(intern.resolve((*result.key.extra_keys)[1].first) == "step");
        CHECK(intern.resolve((*result.key.extra_keys)[1].second) == "42");
    }

    TEST_CASE("key roundtrip - map type preserved") {
        auto& intern = aggregation_intern();
        AggregationKey key;
        key.cat_id = intern.get_or_insert("CAT");
        key.name_id = intern.get_or_insert("NAME");
        key.pid = 1;
        key.tid = 1;
        key.time_bucket = 1000000;

        for (auto mt :
             {AggMapType::EVENT, AggMapType::PROFILE, AggMapType::SYSTEM}) {
            auto data = serialize_agg_key(0, mt, key);
            auto result = deserialize_agg_key(data);
            CHECK(result.map_type == mt);
        }
    }

    TEST_CASE("key sort order - shard prefix") {
        auto& intern = aggregation_intern();
        AggregationKey a, b;
        a.cat_id = intern.get_or_insert("AAA");
        a.name_id = intern.get_or_insert("aaa");
        a.pid = 1;
        a.tid = 1;
        a.time_bucket = 1000000;

        b = a;
        b.cat_id = intern.get_or_insert("BBB");
        auto ka = serialize_agg_key(0, AggMapType::EVENT, a);
        auto kb = serialize_agg_key(0, AggMapType::EVENT, b);
        CHECK(ka < kb);
    }

    TEST_CASE("key uniqueness - different time_bucket") {
        auto& intern = aggregation_intern();
        AggregationKey a, b;
        a.cat_id = intern.get_or_insert("AAA");
        a.name_id = intern.get_or_insert("aaa");
        a.pid = 1;
        a.tid = 1;
        a.time_bucket = 1000000;

        b = a;
        b.time_bucket = 2000000;

        auto ka = serialize_agg_key(0, AggMapType::EVENT, a);
        auto kb = serialize_agg_key(0, AggMapType::EVENT, b);
        CHECK(ka != kb);
    }

    TEST_CASE("value roundtrip - basic") {
        AggregationMetrics m;
        m.count = 100;
        m.duration.count = 100;
        m.duration.total = 5000;
        m.duration.min = 10;
        m.duration.max = 200;
        m.duration.mean = 50.0;
        m.duration.m2 = 1234.5;
        m.size.count = 50;
        m.size.total = 2000;
        m.size.min = 5;
        m.size.max = 100;
        m.size.mean = 40.0;
        m.ts = 1000000;
        m.te = 2000000;
        m.parent_pid = 42;

        auto data = serialize_agg_value(m);
        auto m2 = deserialize_agg_value(data);

        CHECK(m2.count == 100);
        CHECK(m2.duration.count == 100);
        CHECK(m2.duration.total == 5000);
        CHECK(m2.duration.min == 10);
        CHECK(m2.duration.max == 200);
        CHECK(m2.duration.mean == doctest::Approx(50.0));
        CHECK(m2.duration.m2 == doctest::Approx(1234.5));
        CHECK(m2.size.count == 50);
        CHECK(m2.size.total == 2000);
        CHECK(m2.ts == 1000000);
        CHECK(m2.te == 2000000);
        CHECK(m2.parent_pid == 42);
        CHECK(m2.custom_metrics == nullptr);
    }

    TEST_CASE("value full-view fast path - mean/m2 endianness") {
        // Regression: parse_agg_value_full_view::read_f64 must decode doubles
        // big-endian to match put_double on the write side. A little-endian
        // read byte-swaps mean/m2, corrupting the mean/stddev columns that
        // iter_aggregation / dfanalyzer emit.
        AggregationMetrics m;
        m.count = 100;
        m.duration.count = 100;
        m.duration.total = 5000;
        m.duration.min = 10;
        m.duration.max = 200;
        m.duration.mean = 12345.678;
        m.duration.m2 = 98765.4321;
        m.size.count = 100;
        m.size.total = 8000;
        m.size.mean = 42.5;
        m.size.m2 = 271828.1828;
        m.offset.count = 100;
        m.offset.mean = 3.14159;
        m.offset.m2 = 161803.398;

        auto data = serialize_agg_value(m);

        AggMetricsFullView fv;
        REQUIRE(parse_agg_value_full_view(data, fv));

        CHECK(fv.count == 100);
        CHECK(fv.dur_mean == doctest::Approx(12345.678));
        CHECK(fv.dur_m2 == doctest::Approx(98765.4321));
        CHECK(fv.size_mean == doctest::Approx(42.5));
        CHECK(fv.size_m2 == doctest::Approx(271828.1828));
        CHECK(fv.offset_mean == doctest::Approx(3.14159));
        CHECK(fv.offset_m2 == doctest::Approx(161803.398));
    }

    TEST_CASE("value roundtrip - with custom metrics") {
        AggregationMetrics m;
        m.count = 10;
        m.duration.count = 10;
        m.duration.total = 500;
        m.duration.min = 10;
        m.duration.max = 100;
        m.duration.mean = 50.0;
        m.ts = 100;
        m.te = 200;
        m.custom_metrics = std::make_unique<CustomMetricsMap>();
        MetricStats cm;
        cm.count = 5;
        cm.total = 250;
        cm.min = 20;
        cm.max = 80;
        cm.mean = 50.0;
        cm.m2 = 100.0;
        m.custom_metrics->emplace("offset", std::move(cm));

        auto data = serialize_agg_value(m);
        auto m2 = deserialize_agg_value(data);

        REQUIRE(m2.custom_metrics != nullptr);
        REQUIRE(m2.custom_metrics->count("offset") == 1);
        auto& cm2 = m2.custom_metrics->at("offset");
        CHECK(cm2.count == 5);
        CHECK(cm2.total == 250);
        CHECK(cm2.min == 20);
        CHECK(cm2.max == 80);
        CHECK(cm2.mean == doctest::Approx(50.0));
    }

    TEST_CASE("value roundtrip - with sketch") {
        AggregationMetrics m;
        m.count = 3;
        m.duration.count = 3;
        m.duration.total = 300;
        m.duration.min = 50;
        m.duration.max = 150;
        m.duration.mean = 100.0;
        m.ts = 100;
        m.te = 200;

        m.duration.update(50, true);
        m.duration.update(100, true);
        m.duration.update(150, true);

        REQUIRE(m.duration.sketch != nullptr);

        auto data = serialize_agg_value(m);
        auto m2 = deserialize_agg_value(data);

        REQUIRE(m2.duration.sketch != nullptr);
        CHECK(m2.duration.sketch->count() == m.duration.sketch->count());
    }
}
