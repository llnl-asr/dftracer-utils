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

namespace {

struct ClassNginx {
    static constexpr dstd::StringView id = "class_nginx";
    index::Field<std::int64_t, "status"> status;
    index::Field<double, "request_time",
                 index::DurationRole<index::TimeUnit::S>>
        request_time;
    index::Field<dstd::Optional<dstd::String>, "meta.host"> host;
    index::Field<dstd::String, "upstream", index::AlwaysIndex> upstream;
    index::Field<dstd::Optional<index::Json>, "tags"> tags;
};

struct ClassStep {
    static constexpr dstd::StringView id = "class_step";
    using extends = index::schemas::DFTracer;
    index::Field<std::int64_t, "args.step"> step;
};

struct ClassWeb {
    static constexpr dstd::StringView id = "class_web";
    static constexpr dstd::StringView SOURCE =
        "errors = where status >= 500; def slow(t) = latency > t";
    index::Field<std::int64_t, "status"> status;
    index::Field<double, "latency"> latency;
};

struct ClassTimed {
    static constexpr dstd::StringView id = "class_timed";
    index::Field<std::uint64_t, "ts_ms", index::TimeRole<index::TimeUnit::Ms>>
        ts_ms;
    index::Field<dstd::String, "worker", index::EntityRole> worker;
    index::Field<dstd::Optional<bool>, "ok"> ok;
    index::Field<dstd::Optional<std::int64_t>, "thread", index::LaneRole>
        thread;
    index::Field<dstd::Optional<dstd::String>, "op", index::NameRole> op;
};

}  // namespace

TEST_SUITE("SchemaClass") {
    TEST_CASE("a class and the equal YAML spec are one definition") {
        const auto& s = index::register_schema<ClassNginx>("test_schema_class");
        CHECK(s.id == "class_nginx");
        CHECK(s.require ==
              dstd::Vector<dstd::String>{"status", "request_time", "upstream"});
        CHECK(s.roles.duration == "request_time");
        CHECK(s.roles.duration_unit == index::TimeUnit::S);
        CHECK(s.always_index ==
              dstd::Vector<dstd::String>{"request_time", "upstream"});
        REQUIRE(s.field_at("tags") != nullptr);
        CHECK(s.field_at("tags")->type == index::FieldType::Json);
        CHECK(s.field_at("meta.host")->optional);
        CHECK(&index::register_schema(
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
        const auto& s = index::register_schema<ClassStep>("test_schema_class");
        CHECK(s.decoder == index::Decoder::Dftracer);
        CHECK(s.require ==
              dstd::Vector<dstd::String>{"ph", "name", "ts", "args.step"});
    }

    TEST_CASE("member types map to field types") {
        const auto spec = index::schema_spec<ClassTimed>();
        REQUIRE(spec.fields.size() == 5);
        CHECK(spec.fields[0].type == index::FieldType::Int);
        CHECK(spec.fields[0].role == index::Role::Time);
        CHECK(spec.fields[0].unit == index::TimeUnit::Ms);
        CHECK(spec.fields[1].role == index::Role::Entity);
        CHECK(spec.fields[2].type == index::FieldType::Bool);
        CHECK(spec.fields[2].optional);
        CHECK(spec.fields[3].role == index::Role::Lane);
        CHECK(spec.fields[4].role == index::Role::Name);
        CHECK(spec.extends == "generic");
    }

    TEST_CASE("a class declares its source") {
        CHECK(index::schema_spec<ClassWeb>().source ==
              "errors = where status >= 500; def slow(t) = latency > t");
        const auto& s = index::register_schema<ClassWeb>("test_schema_class");
        CHECK(s.source ==
              "errors = where status >= 500;\ndef slow(t) = latency > t");
    }
}
