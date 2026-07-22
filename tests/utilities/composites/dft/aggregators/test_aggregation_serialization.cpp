#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_intern.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::utilities::composites::dft::aggregators;

TEST_SUITE("AggregationSerialization") {
    TEST_CASE("key roundtrip - basic") {
        auto table = make_intern_table();
        auto& intern = table->intern;
        AggregationKey key;
        key.cat_id = intern.get_or_insert("POSIX");
        key.name_id = intern.get_or_insert("read");
        key.pid = 12345;
        key.tid = 67890;
        key.hhash_id = intern.get_or_insert("abc123");
        key.fhash = 0xdef456ULL;
        key.time_bucket = 5000000;

        auto data = serialize_agg_key(42, AggMapType::EVENT, key, intern);
        auto result = deserialize_agg_key(data);

        CHECK(result.map_type == AggMapType::EVENT);
        CHECK(result.key.cat(intern) == "POSIX");
        CHECK(result.key.name(intern) == "read");
        CHECK(result.key.pid == key.pid);
        CHECK(result.key.tid == key.tid);
        CHECK(result.key.hhash(intern) == "abc123");
        CHECK(result.key.fhash_inline);
        CHECK(result.key.fhash == 0xdef456ULL);
        CHECK(result.key.time_bucket == key.time_bucket);
        CHECK(result.key.extra_keys == nullptr);
    }

    TEST_CASE("key roundtrip - file hash travels in the key") {
        auto table = make_intern_table();
        auto& intern = table->intern;

        SUBCASE("canonical hash is inline, not interned") {
            AggregationKey key;
            key.cat_id = intern.get_or_insert("posix");
            key.name_id = intern.get_or_insert("read");
            key.fhash = 0xf07c4ebf132e3799ULL;
            const auto before = intern.entry_count();

            auto data = serialize_agg_key(0, AggMapType::EVENT, key, intern);
            auto result = deserialize_agg_key(data);
            CHECK(result.key.fhash_inline);
            CHECK(result.key.fhash == key.fhash);
            CHECK(intern.entry_count() == before);

            AggKeyView view;
            REQUIRE(parse_agg_key_view(data, intern, view));
            char buf[dftracer::utils::hash::HEX64_DIGITS];
            CHECK(fhash_text(view, buf) == "f07c4ebf132e3799");
        }

        SUBCASE("a hash outside that form keeps its text") {
            std::string out;
            serialize_agg_key_into(out, 0, AggMapType::EVENT, "posix", "read",
                                   1, 2, "", "NOT-A-HASH", 0, intern);
            AggKeyView view;
            REQUIRE(parse_agg_key_view(out, intern, view));
            CHECK_FALSE(view.fhash_inline);
            char buf[dftracer::utils::hash::HEX64_DIGITS];
            CHECK(fhash_text(view, buf) == "NOT-A-HASH");
        }

        SUBCASE("no file hash resolves to empty") {
            std::string out;
            serialize_agg_key_into(out, 0, AggMapType::EVENT, "posix", "read",
                                   1, 2, "", "", 0, intern);
            AggKeyView view;
            REQUIRE(parse_agg_key_view(out, intern, view));
            char buf[dftracer::utils::hash::HEX64_DIGITS];
            CHECK(fhash_text(view, buf).empty());
        }
    }

    TEST_CASE("key roundtrip - with extra keys") {
        auto table = make_intern_table();
        auto& intern = table->intern;
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

        auto data = serialize_agg_key(99, AggMapType::PROFILE, key, intern);
        auto result = deserialize_agg_key(data);

        CHECK(result.map_type == AggMapType::PROFILE);
        CHECK(result.key.cat(intern) == "MPI");
        REQUIRE(result.key.extra_keys != nullptr);
        REQUIRE(result.key.extra_keys->size() == 2);
        CHECK(intern.resolve((*result.key.extra_keys)[0].first) == "epoch");
        CHECK(intern.resolve((*result.key.extra_keys)[0].second) == "1");
        CHECK(intern.resolve((*result.key.extra_keys)[1].first) == "step");
        CHECK(intern.resolve((*result.key.extra_keys)[1].second) == "42");
    }

    TEST_CASE("key roundtrip - map type preserved") {
        auto table = make_intern_table();
        auto& intern = table->intern;
        AggregationKey key;
        key.cat_id = intern.get_or_insert("CAT");
        key.name_id = intern.get_or_insert("NAME");
        key.pid = 1;
        key.tid = 1;
        key.time_bucket = 1000000;

        for (auto mt :
             {AggMapType::EVENT, AggMapType::PROFILE, AggMapType::SYSTEM}) {
            auto data = serialize_agg_key(0, mt, key, intern);
            auto result = deserialize_agg_key(data);
            CHECK(result.map_type == mt);
        }
    }

    TEST_CASE("key sort order - shard prefix") {
        auto table = make_intern_table();
        auto& intern = table->intern;
        AggregationKey a, b;
        a.cat_id = intern.get_or_insert("AAA");
        a.name_id = intern.get_or_insert("aaa");
        a.pid = 1;
        a.tid = 1;
        a.time_bucket = 1000000;

        b = a;
        b.cat_id = intern.get_or_insert("BBB");
        auto ka = serialize_agg_key(0, AggMapType::EVENT, a, intern);
        auto kb = serialize_agg_key(0, AggMapType::EVENT, b, intern);
        CHECK(ka < kb);
    }

    TEST_CASE("key uniqueness - different time_bucket") {
        auto table = make_intern_table();
        auto& intern = table->intern;
        AggregationKey a, b;
        a.cat_id = intern.get_or_insert("AAA");
        a.name_id = intern.get_or_insert("aaa");
        a.pid = 1;
        a.tid = 1;
        a.time_bucket = 1000000;

        b = a;
        b.time_bucket = 2000000;

        auto ka = serialize_agg_key(0, AggMapType::EVENT, a, intern);
        auto kb = serialize_agg_key(0, AggMapType::EVENT, b, intern);
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

TEST_SUITE("AggregationSerialization - distinct files") {
    TEST_CASE("value round trip keeps the sketch") {
        for (int n : {0, 20, 5000}) {
            AggregationMetrics m;
            m.count = 7;
            m.ts = 10;
            m.te = 99;
            for (int i = 0; i < n; ++i)
                m.distinct_files.add("f" + std::to_string(i));

            auto blob = serialize_agg_value(m);
            auto back = deserialize_agg_value(blob);
            CHECK(back.count == 7);
            if (n == 0) {
                CHECK(back.distinct_files.empty());
            } else {
                CHECK(back.distinct_files.estimate() ==
                      m.distinct_files.estimate());
            }

            AggMetricsView view;
            REQUIRE(parse_agg_value_view(blob, view));
            CHECK(view.count == 7);
            CHECK(view.distinct_files == m.distinct_files.estimate());
        }
    }

    TEST_CASE("view reports no distinct files for a value without a sketch") {
        AggregationMetrics m;
        m.count = 3;
        m.ts = 1;
        m.te = 2;
        auto blob = serialize_agg_value(m);
        AggMetricsView view;
        REQUIRE(parse_agg_value_view(blob, view));
        CHECK(view.distinct_files == 0);
    }

    TEST_CASE("custom metrics do not confuse the sketch position") {
        AggregationMetrics m;
        m.count = 5;
        m.custom_metrics = std::make_unique<CustomMetricsMap>();
        MetricStats ms(0.01);
        ms.count = 2;
        ms.total = 40;
        ms.min = 15;
        ms.max = 25;
        m.custom_metrics->emplace("bandwidth", ms);
        for (int i = 0; i < 30; ++i)
            m.distinct_files.add("f" + std::to_string(i));

        auto blob = serialize_agg_value(m);
        AggMetricsView view;
        REQUIRE(parse_agg_value_view(blob, view));
        CHECK(view.count == 5);
        CHECK(view.distinct_files == 30);
    }
}
