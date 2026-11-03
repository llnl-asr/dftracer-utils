// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/schema_class.h>
#include <doctest/doctest.h>

#include <cstdint>

using namespace dftracer::utils;

namespace ix = dftracer::utils::index;

namespace {

struct ClassNginx {
    static constexpr dftracer::utils::dstd::StringView id = "class_nginx";
    ix::Field<std::int64_t, "status"> status;
    ix::Field<double, "request_time", ix::DurationRole<ix::TimeUnit::S>>
        request_time;
    ix::Field<dftracer::utils::dstd::Optional<dstd::String>, "meta.host"> host;
    ix::Field<dstd::String, "upstream", ix::AlwaysIndex> upstream;
    ix::Field<dftracer::utils::dstd::Optional<ix::Json>, "tags"> tags;
};

struct ClassStep {
    static constexpr dftracer::utils::dstd::StringView id = "class_step";
    using extends = ix::schemas::DFTracer;
    ix::Field<std::int64_t, "args.step"> step;
};

struct ClassWeb {
    static constexpr dftracer::utils::dstd::StringView id = "class_web";
    static constexpr dftracer::utils::dstd::StringView SOURCE =
        "errors = where status >= 500; def slow(t) = latency > t";
    ix::Field<std::int64_t, "status"> status;
    ix::Field<double, "latency"> latency;
};

struct ClassTimed {
    static constexpr dftracer::utils::dstd::StringView id = "class_timed";
    ix::Field<std::uint64_t, "ts_ms", ix::TimeRole<ix::TimeUnit::Ms>> ts_ms;
    ix::Field<dstd::String, "worker", ix::EntityRole> worker;
    ix::Field<dftracer::utils::dstd::Optional<bool>, "ok"> ok;
    ix::Field<dftracer::utils::dstd::Optional<std::int64_t>, "thread",
              ix::LaneRole>
        thread;
    ix::Field<dftracer::utils::dstd::Optional<dstd::String>, "op", ix::NameRole>
        op;
};

}  // namespace

TEST_SUITE("SchemaClass") {
    TEST_CASE("a class and the equal YAML spec are one definition") {
        const auto& s = ix::register_schema<ClassNginx>("test_schema_class");
        CHECK(s.id == "class_nginx");
        CHECK(s.require ==
              dstd::Vector<dstd::String>{"status", "request_time", "upstream"});
        CHECK(s.roles.duration == "request_time");
        CHECK(s.roles.duration_unit == ix::TimeUnit::S);
        CHECK(s.always_index ==
              dstd::Vector<dstd::String>{"request_time", "upstream"});
        REQUIRE(s.field_at("tags") != nullptr);
        CHECK(s.field_at("tags")->type == ix::FieldType::Json);
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
        CHECK(s.decoder == ix::Decoder::Dftracer);
        CHECK(s.require ==
              dstd::Vector<dstd::String>{"ph", "name", "ts", "args.step"});
    }

    TEST_CASE("member types map to field types") {
        const auto spec = ix::schema_spec<ClassTimed>();
        REQUIRE(spec.fields.size() == 5);
        CHECK(spec.fields[0].type == ix::FieldType::Int);
        CHECK(spec.fields[0].role == ix::Role::Time);
        CHECK(spec.fields[0].unit == ix::TimeUnit::Ms);
        CHECK(spec.fields[1].role == ix::Role::Entity);
        CHECK(spec.fields[2].type == ix::FieldType::Bool);
        CHECK(spec.fields[2].optional);
        CHECK(spec.fields[3].role == ix::Role::Lane);
        CHECK(spec.fields[4].role == ix::Role::Name);
        CHECK(spec.extends == "generic");
    }

    TEST_CASE("a class declares its source") {
        CHECK(ix::schema_spec<ClassWeb>().source ==
              "errors = where status >= 500; def slow(t) = latency > t");
        const auto& s = ix::register_schema<ClassWeb>("test_schema_class");
        CHECK(s.source ==
              "errors = where status >= 500;\ndef slow(t) = latency > t");
    }
}
