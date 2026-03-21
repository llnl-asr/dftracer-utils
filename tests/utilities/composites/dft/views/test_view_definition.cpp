#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>
#include <doctest/doctest.h>

#include <string>

using namespace dftracer::utils::utilities::composites::dft::views;

TEST_SUITE("ViewDefinition") {
    TEST_CASE("ViewDefinition - Fluent builders") {
        ViewDefinition view;
        view.with_name("test_view")
            .with_description("A test view")
            .with_query(R"(cat == "POSIX")")
            .with_include_metadata(false);

        CHECK(view.name == "test_view");
        CHECK(view.description == "A test view");
        CHECK(view.query.has_value());
        CHECK(view.include_metadata == false);
    }

    TEST_CASE("ViewDefinition - include_metadata defaults to true") {
        ViewDefinition view;
        CHECK(view.include_metadata == true);
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
            .with_query(R"(cat in ["POSIX", "STDIO"] and dur >= 10)")
            .with_include_metadata(false);

        std::string json = original.to_json();
        CHECK(!json.empty());

        auto restored = ViewDefinition::from_json(json);
        CHECK(restored.name == "test_roundtrip");
        CHECK(restored.description == "Round-trip test");
        CHECK(restored.query.has_value());
        CHECK(restored.include_metadata == false);
    }

    TEST_CASE("ViewDefinition - JSON round-trip with no query") {
        ViewDefinition original;
        original.with_name("empty").with_description("No query");

        std::string json = original.to_json();
        auto restored = ViewDefinition::from_json(json);

        CHECK(restored.name == "empty");
        CHECK_FALSE(restored.query.has_value());
    }
}
