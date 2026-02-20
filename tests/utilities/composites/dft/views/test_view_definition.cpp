#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>
#include <doctest/doctest.h>

#include <string>

using namespace dftracer::utils::utilities::composites::dft::views;

TEST_SUITE("ViewDefinition") {
    TEST_CASE("ViewPredicate - Fluent builders") {
        ViewPredicate pred;
        pred.with_bloom_dim("name", {"read", "write"})
            .with_bloom_dim("cat", {"POSIX"})
            .with_time_range(1000.0, 2000.0)
            .with_min_duration(50.0)
            .with_max_duration(10000.0);

        CHECK(pred.bloom_dims.size() == 2);
        CHECK(pred.bloom_dims["name"].size() == 2);
        CHECK(pred.bloom_dims["name"][0] == "read");
        CHECK(pred.bloom_dims["name"][1] == "write");
        CHECK(pred.bloom_dims["cat"].size() == 1);
        CHECK(pred.bloom_dims["cat"][0] == "POSIX");

        REQUIRE(pred.time_range.has_value());
        CHECK(pred.time_range->first == 1000.0);
        CHECK(pred.time_range->second == 2000.0);

        REQUIRE(pred.min_duration_us.has_value());
        CHECK(*pred.min_duration_us == 50.0);

        REQUIRE(pred.max_duration_us.has_value());
        CHECK(*pred.max_duration_us == 10000.0);
    }

    TEST_CASE("ViewPredicate - has_bloom_dims") {
        ViewPredicate pred;
        CHECK_FALSE(pred.has_bloom_dims());

        pred.with_bloom_dim("name", {"read"});
        CHECK(pred.has_bloom_dims());
    }

    TEST_CASE("ViewPredicate - has_event_filters") {
        ViewPredicate pred;
        CHECK_FALSE(pred.has_event_filters());

        SUBCASE("time_range only") {
            pred.with_time_range(0.0, 100.0);
            CHECK(pred.has_event_filters());
        }

        SUBCASE("min_duration only") {
            pred.with_min_duration(10.0);
            CHECK(pred.has_event_filters());
        }

        SUBCASE("max_duration only") {
            pred.with_max_duration(500.0);
            CHECK(pred.has_event_filters());
        }
    }

    TEST_CASE("ViewDefinition - Fluent builders") {
        ViewDefinition view;
        ViewPredicate pred;
        pred.with_bloom_dim("cat", {"POSIX"});

        view.with_name("test_view")
            .with_description("A test view")
            .with_predicate(std::move(pred))
            .with_include_metadata(false);

        CHECK(view.name == "test_view");
        CHECK(view.description == "A test view");
        CHECK(view.predicates.size() == 1);
        CHECK(view.include_metadata == false);
    }

    TEST_CASE("ViewDefinition - include_metadata defaults to true") {
        ViewDefinition view;
        CHECK(view.include_metadata == true);
    }

    TEST_CASE("ViewDefinition - Predefined io_view") {
        auto view = ViewDefinition::io_view();

        CHECK(view.name == "io");
        CHECK(view.predicates.size() == 1);

        const auto& pred = view.predicates[0];
        CHECK(pred.bloom_dims.count("cat") == 1);
        CHECK(pred.bloom_dims.count("name") == 1);

        // Check categories
        auto& cats = pred.bloom_dims.at("cat");
        bool has_posix = false, has_stdio = false;
        for (const auto& c : cats) {
            if (c == "POSIX") has_posix = true;
            if (c == "STDIO") has_stdio = true;
        }
        CHECK(has_posix);
        CHECK(has_stdio);

        // Check some I/O operation names
        auto& names = pred.bloom_dims.at("name");
        bool has_read = false, has_write = false;
        for (const auto& n : names) {
            if (n == "read") has_read = true;
            if (n == "write") has_write = true;
        }
        CHECK(has_read);
        CHECK(has_write);
    }

    TEST_CASE("ViewDefinition - Predefined compute_view") {
        auto view = ViewDefinition::compute_view();

        CHECK(view.name == "compute");
        CHECK(view.predicates.size() == 1);

        const auto& pred = view.predicates[0];
        CHECK(pred.bloom_dims.count("cat") == 1);

        auto& cats = pred.bloom_dims.at("cat");
        bool has_compute = false, has_comm = false;
        for (const auto& c : cats) {
            if (c == "compute") has_compute = true;
            if (c == "comm") has_comm = true;
        }
        CHECK(has_compute);
        CHECK(has_comm);
    }

    TEST_CASE("ViewDefinition - Predefined dlio_view") {
        auto view = ViewDefinition::dlio_view();

        CHECK(view.name == "dlio");
        CHECK(view.predicates.size() == 1);

        const auto& pred = view.predicates[0];
        auto& cats = pred.bloom_dims.at("cat");
        bool has_dlio = false, has_data_loader = false;
        for (const auto& c : cats) {
            if (c == "dlio_benchmark") has_dlio = true;
            if (c == "data_loader") has_data_loader = true;
        }
        CHECK(has_dlio);
        CHECK(has_data_loader);
    }

    TEST_CASE("ViewDefinition - JSON round-trip") {
        ViewDefinition original;
        original.with_name("test_roundtrip")
            .with_description("Round-trip test");

        ViewPredicate pred1;
        pred1.with_bloom_dim("cat", {"POSIX", "STDIO"})
            .with_bloom_dim("name", {"read", "write"})
            .with_time_range(1000.0, 5000.0)
            .with_min_duration(10.0)
            .with_max_duration(9999.0);
        original.with_predicate(std::move(pred1));

        ViewPredicate pred2;
        pred2.with_bloom_dim("cat", {"compute"});
        original.with_predicate(std::move(pred2));

        // Serialize
        std::string json = original.to_json();
        CHECK(!json.empty());

        // Deserialize
        auto restored = ViewDefinition::from_json(json);

        CHECK(restored.name == "test_roundtrip");
        CHECK(restored.description == "Round-trip test");
        REQUIRE(restored.predicates.size() == 2);

        // First predicate
        const auto& p1 = restored.predicates[0];
        CHECK(p1.bloom_dims.count("cat") == 1);
        CHECK(p1.bloom_dims.at("cat").size() == 2);
        CHECK(p1.bloom_dims.count("name") == 1);
        CHECK(p1.bloom_dims.at("name").size() == 2);

        REQUIRE(p1.time_range.has_value());
        CHECK(p1.time_range->first == doctest::Approx(1000.0));
        CHECK(p1.time_range->second == doctest::Approx(5000.0));

        REQUIRE(p1.min_duration_us.has_value());
        CHECK(*p1.min_duration_us == doctest::Approx(10.0));

        REQUIRE(p1.max_duration_us.has_value());
        CHECK(*p1.max_duration_us == doctest::Approx(9999.0));

        // Second predicate
        const auto& p2 = restored.predicates[1];
        CHECK(p2.bloom_dims.count("cat") == 1);
        CHECK(p2.bloom_dims.at("cat").size() == 1);
        CHECK(p2.bloom_dims.at("cat")[0] == "compute");
        CHECK_FALSE(p2.time_range.has_value());
        CHECK_FALSE(p2.min_duration_us.has_value());
        CHECK_FALSE(p2.max_duration_us.has_value());
    }

    TEST_CASE("ViewDefinition - JSON round-trip with no event filters") {
        ViewDefinition original;
        original.with_name("simple").with_description("Simple view");

        ViewPredicate pred;
        pred.with_bloom_dim("name", {"open"});
        original.with_predicate(std::move(pred));

        std::string json = original.to_json();
        auto restored = ViewDefinition::from_json(json);

        CHECK(restored.name == "simple");
        REQUIRE(restored.predicates.size() == 1);
        CHECK(restored.predicates[0].bloom_dims.at("name")[0] == "open");
        CHECK_FALSE(restored.predicates[0].time_range.has_value());
    }

    TEST_CASE("resolve_bloom_dimension - aliases") {
        CHECK(resolve_bloom_dimension("host") == "hhash");
        CHECK(resolve_bloom_dimension("file") == "fhash");
        CHECK(resolve_bloom_dimension("script") == "shash");
        CHECK(resolve_bloom_dimension("name") == "name");
        CHECK(resolve_bloom_dimension("cat") == "cat");
        CHECK(resolve_bloom_dimension("unknown") == "unknown");
    }
}
