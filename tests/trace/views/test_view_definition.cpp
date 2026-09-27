#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/trace/views/view_definition.h>
#include <doctest/doctest.h>

#include <string>

using namespace dftracer::utils::trace::views;

TEST_SUITE("ViewDefinition") {
    TEST_CASE("ViewDefinition - Fluent builders") {
        ViewDefinition view;
        view.with_name("test_view")
            .with_description("A test view")
            .with_query(R"(cat == "POSIX")");

        CHECK(view.name == "test_view");
        CHECK(view.description == "A test view");
        CHECK(view.query.has_value());
    }

    TEST_CASE("ViewDefinition - metadata records are left out by default") {
        ViewDefinition view;
        CHECK_FALSE(view.include_metadata);
    }

    TEST_CASE("ViewDefinition - Predefined io_view") {
        auto view = ViewDefinition::io_view();
        CHECK(view.name == "io");
        CHECK(view.query.has_value());
    }

    TEST_CASE("ViewDefinition - Predefined compute_view") {
        auto view = ViewDefinition::compute_view();
        CHECK(view.name == "compute");
        CHECK(view.query.has_value());
    }

    TEST_CASE("ViewDefinition - Predefined dlio_view") {
        auto view = ViewDefinition::dlio_view();
        CHECK(view.name == "dlio");
        CHECK(view.query.has_value());
    }

    TEST_CASE("ViewDefinition - JSON round-trip") {
        ViewDefinition original;
        original.with_name("test_roundtrip")
            .with_description("Round-trip test")
            .with_query(R"(cat in ["POSIX", "STDIO"] and dur >= 10)");
        original.include_metadata = true;
        original.metadata_records = true;

        std::string json = original.to_json();
        CHECK(!json.empty());

        auto restored = ViewDefinition::from_json(json);
        CHECK(restored.name == "test_roundtrip");
        CHECK(restored.description == "Round-trip test");
        CHECK(restored.query.has_value());
        CHECK(restored.include_metadata);
        CHECK(restored.metadata_records);
    }

    TEST_CASE("ViewDefinition - JSON round-trip with no query") {
        ViewDefinition original;
        original.with_name("empty").with_description("No query");

        std::string json = original.to_json();
        auto restored = ViewDefinition::from_json(json);

        CHECK(restored.name == "empty");
        CHECK_FALSE(restored.query.has_value());
        CHECK_FALSE(restored.metadata_records);
    }
}
