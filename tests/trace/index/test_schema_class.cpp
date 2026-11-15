// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/record_schema.h>
#include <dftracer/utils/trace/index/schema_class.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <cstdint>

using namespace dftracer::utils;

namespace {

struct ClassNginx {
    static constexpr dstd::StringView id = "class_nginx";
    trace::index::Field<std::int64_t, "status"> status;
    trace::index::Field<double, "request_time",
                        trace::index::DurationRole<trace::index::TimeUnit::S>>
        request_time;
    trace::index::Field<dstd::Optional<dstd::String>, "meta.host"> host;
    trace::index::Field<dstd::String, "upstream", trace::index::AlwaysIndex>
        upstream;
    trace::index::Field<dstd::Optional<trace::index::Json>, "tags"> tags;
};

struct ClassStep {
    static constexpr dstd::StringView id = "class_step";
    using Extends = trace::index::schemas::DFTracer;
    trace::index::Field<std::int64_t, "args.step"> step;
};

struct ClassWeb {
    static constexpr dstd::StringView id = "class_web";
    static constexpr dstd::StringView SOURCE =
        "errors = where status >= 500; def slow(t) = latency > t";
    trace::index::Field<std::int64_t, "status"> status;
    trace::index::Field<double, "latency"> latency;
};

struct ClassTimed {
    static constexpr dstd::StringView id = "class_timed";
    trace::index::Field<std::uint64_t, "ts_ms",
                        trace::index::TimeRole<trace::index::TimeUnit::Ms>>
        ts_ms;
    trace::index::Field<dstd::String, "worker", trace::index::EntityRole>
        worker;
    trace::index::Field<dstd::Optional<bool>, "ok"> ok;
    trace::index::Field<dstd::Optional<std::int64_t>, "thread",
                        trace::index::LaneRole>
        thread;
    trace::index::Field<dstd::Optional<dstd::String>, "op",
                        trace::index::NameRole>
        op;
};

}  // namespace

TEST_SUITE("SchemaClass") {
    TEST_CASE("a class and the equal YAML spec are one definition") {
        const auto& s = dftu_utils_test::ok_ref(
            trace::index::register_schema<ClassNginx>("test_schema_class"));
        CHECK(s.id == "class_nginx");
        CHECK(s.require ==
              dstd::Vector<dstd::String>{"status", "request_time", "upstream"});
        CHECK(s.roles.duration == "request_time");
        CHECK(s.roles.duration_unit == trace::index::TimeUnit::S);
        CHECK(s.always_index ==
              dstd::Vector<dstd::String>{"request_time", "upstream"});
        REQUIRE(s.field_at("tags") != nullptr);
        CHECK(s.field_at("tags")->type == trace::index::FieldType::Json);
        CHECK(s.field_at("meta.host")->optional);
        CHECK(&dftu_utils_test::ok_ref(trace::index::register_schema(
                  "id: class_nginx\n"
                  "fields:\n"
                  "  tags: {type: json, optional: true}\n"
                  "  upstream: {type: string, always_index: true}\n"
                  "  meta.host: {type: string, optional: true}\n"
                  "  request_time: {type: float, role: duration, unit: s}\n"
                  "  status: {type: int}\n",
                  "yaml")) == &s);
    }

    TEST_CASE("a class extends another schema") {
        const auto& s = dftu_utils_test::ok_ref(
            trace::index::register_schema<ClassStep>("test_schema_class"));
        CHECK(s.decoder == trace::index::Decoder::Dftracer);
        CHECK(s.require ==
              dstd::Vector<dstd::String>{"ph", "name", "ts", "args.step"});
    }

    TEST_CASE("member types map to field types") {
        const auto spec = trace::index::schema_spec<ClassTimed>();
        REQUIRE(spec.fields.size() == 5);
        CHECK(spec.fields[0].type == trace::index::FieldType::Int);
        CHECK(spec.fields[0].role == trace::index::Role::Time);
        CHECK(spec.fields[0].unit == trace::index::TimeUnit::Ms);
        CHECK(spec.fields[1].role == trace::index::Role::Entity);
        CHECK(spec.fields[2].type == trace::index::FieldType::Bool);
        CHECK(spec.fields[2].optional);
        CHECK(spec.fields[3].role == trace::index::Role::Lane);
        CHECK(spec.fields[4].role == trace::index::Role::Name);
        CHECK(spec.extends == "generic");
    }

    TEST_CASE("a class declares its source") {
        CHECK(trace::index::schema_spec<ClassWeb>().source ==
              "errors = where status >= 500; def slow(t) = latency > t");
        const auto& s = dftu_utils_test::ok_ref(
            trace::index::register_schema<ClassWeb>("test_schema_class"));
        CHECK(s.source ==
              "errors = where status >= 500;\ndef slow(t) = latency > t");
    }
}
