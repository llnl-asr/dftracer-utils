#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/schema_class.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ix = dftracer::utils::index;

namespace {

struct ClassNginx {
    static constexpr std::string_view id = "class_nginx";
    ix::Field<std::int64_t, "status"> status;
    ix::Field<double, "request_time", ix::DurationRole<ix::TimeUnit::S>>
        request_time;
    ix::Field<std::optional<std::string>, "meta.host"> host;
    ix::Field<std::string, "upstream", ix::AlwaysIndex> upstream;
    ix::Field<std::optional<ix::Json>, "tags"> tags;
};

struct ClassStep {
    static constexpr std::string_view id = "class_step";
    using extends = ix::schemas::DFTracer;
    ix::Field<std::int64_t, "args.step"> step;
};

struct ClassTimed {
    static constexpr std::string_view id = "class_timed";
    ix::Field<std::uint64_t, "ts_ms", ix::TimeRole<ix::TimeUnit::MS>> ts_ms;
    ix::Field<std::string, "worker", ix::EntityRole> worker;
    ix::Field<std::optional<bool>, "ok"> ok;
};

}  // namespace

TEST_SUITE("SchemaClass") {
    TEST_CASE("a class and the equal YAML spec are one definition") {
        const auto& s = ix::register_schema<ClassNginx>("test_schema_class");
        CHECK(s.id == "class_nginx");
        CHECK(s.require ==
              std::vector<std::string>{"status", "request_time", "upstream"});
        CHECK(s.roles.duration == "request_time");
        CHECK(s.roles.duration_unit == ix::TimeUnit::S);
        CHECK(s.always_index == std::vector<std::string>{"upstream"});
        REQUIRE(s.field_at("tags") != nullptr);
        CHECK(s.field_at("tags")->type == ix::FieldType::JSON);
        CHECK(s.field_at("meta.host")->optional);
        CHECK(&ix::register_schema(
                  "id: class_nginx\n"
                  "fields:\n"
                  "  tags: {type: json, optional: true}\n"
                  "  upstream: {type: string, always_index: true}\n"
                  "  meta.host: {type: string, optional: true}\n"
                  "  request_time: {type: float, role: duration, unit: s}\n"
                  "  status: {type: int}\n",
                  "yaml") == &s);
    }

    TEST_CASE("a class extends another schema") {
        const auto& s = ix::register_schema<ClassStep>("test_schema_class");
        CHECK(s.decoder == ix::Decoder::DFTRACER);
        CHECK(s.require == std::vector<std::string>{"ph", "name", "args.step"});
    }

    TEST_CASE("member types map to field types") {
        const auto spec = ix::schema_spec<ClassTimed>();
        REQUIRE(spec.fields.size() == 3);
        CHECK(spec.fields[0].type == ix::FieldType::INT);
        CHECK(spec.fields[0].role == ix::Role::TIME);
        CHECK(spec.fields[0].unit == ix::TimeUnit::MS);
        CHECK(spec.fields[1].role == ix::Role::ENTITY);
        CHECK(spec.fields[2].type == ix::FieldType::BOOL);
        CHECK(spec.fields[2].optional);
        CHECK(spec.extends == "generic");
    }
}
