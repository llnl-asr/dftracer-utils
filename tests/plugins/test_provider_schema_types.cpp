// SPDX-License-Identifier: MIT
// dftu_source_vt::schema_types lets a provider declare each column's type
// (dftu_schema_add_field/add_child_field), which ProviderSource::schema()
// reports instead of the TypeId::Unknown every column got before. Exercises
// plain scalars, parameterized types (Timestamp/Decimal128/FixedSizeBinary,
// where the parameters - not just the TypeId - must survive), nested types
// (List/Struct), the additive fallback when a provider declares nothing, and
// LazyFrame::output_schema() end to end over a provider-rooted plan.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/host/provider_source.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>

#include <cstdint>

using namespace dftracer::utils;

namespace {

dftu_dataframe* empty_frame(const dstd::Vector<dstd::String>& names) {
    dstd::Vector<const char*> cnames;
    dstd::Vector<dftu_series*> cols;
    for (const dstd::String& n : names) {
        cnames.push_back(n.c_str());
        std::int64_t v = 0;
        cols.push_back(dftu_series_new_flat(DFTU_TYPE_INT64, &v, 0, nullptr));
    }
    return dftu_dataframe_new(cnames.data(), cols.data(),
                              static_cast<std::int32_t>(cols.size()));
}

struct CursorState {
    dftu_dataframe* frame;
    bool done = false;
};

dftu_status cursor_next(dftu_ctx*, void* self, std::int64_t,
                        dftu_dataframe** out, dftu_task** pending) {
    auto* cs = static_cast<CursorState*>(self);
    *pending = nullptr;
    if (cs->done) {
        *out = nullptr;
        return DFTU_END;
    }
    *out = cs->frame;
    cs->frame = nullptr;
    cs->done = true;
    return DFTU_OK;
}

void cursor_destroy(void* self) {
    auto* cs = static_cast<CursorState*>(self);
    if (cs->frame) dftu_dataframe_free(cs->frame);
    delete cs;
}

const dftu_cursor_vt CURSOR_VT = {sizeof(dftu_cursor_vt),
                                  cursor_next,
                                  cursor_destroy,
                                  nullptr,
                                  nullptr,
                                  nullptr};

// A source over the given names, whose scan() always returns one empty
// morsel. `schema_types` is the vtable's optional type-declaration callback;
// pass nullptr for a provider that declares nothing.
struct Fixture {
    dstd::Vector<dstd::String> names;
    dftu_status (*schema_types)(dftu_ctx*, void*, dftu_schema*);
};

dftu_status fixture_schema(dftu_ctx*, void* self, const char* const** out_names,
                           int32_t* out_count) {
    auto* fx = static_cast<Fixture*>(self);
    static thread_local dstd::Vector<const char*> storage;
    storage.clear();
    for (const dstd::String& n : fx->names) storage.push_back(n.c_str());
    *out_names = storage.data();
    *out_count = static_cast<int32_t>(storage.size());
    return DFTU_OK;
}

dftu_status fixture_scan(dftu_ctx*, void* self, const dftu_scan_request*,
                         int32_t*, void** out_cursor_self,
                         const dftu_cursor_vt** out_vt) {
    auto* fx = static_cast<Fixture*>(self);
    auto* cs = new CursorState{empty_frame(fx->names)};
    *out_cursor_self = cs;
    *out_vt = &CURSOR_VT;
    return DFTU_OK;
}

void fixture_destroy(void*) {}

dstd::SharedPtr<dataframe::Source> make_fixture(Fixture& fx) {
    dftu_source_vt vt{};
    vt.struct_size = sizeof(vt);
    vt.schema = fixture_schema;
    vt.scan = fixture_scan;
    vt.destroy = fixture_destroy;
    vt.schema_types = fx.schema_types;
    return plugins::host::make_provider_source(vt, &fx);
}

dftu_status plain_scalars(dftu_ctx*, void*, dftu_schema* s) {
    dftu_schema_add_field(s, "id", DFTU_TYPE_INT64, 0, DFTU_TIME_UNIT_MICRO,
                          nullptr, 0, 0, 0);
    dftu_schema_add_field(s, "val", DFTU_TYPE_FLOAT64, 1, DFTU_TIME_UNIT_MICRO,
                          nullptr, 0, 0, 0);
    return DFTU_OK;
}

dftu_status parameterized(dftu_ctx*, void*, dftu_schema* s) {
    dftu_schema_add_field(s, "ts", DFTU_TYPE_TIMESTAMP, 1, DFTU_TIME_UNIT_NANO,
                          "UTC", 0, 0, 0);
    dftu_schema_add_field(s, "amount", DFTU_TYPE_DECIMAL128, 1,
                          DFTU_TIME_UNIT_MICRO, nullptr, 38, 9, 0);
    dftu_schema_add_field(s, "digest", DFTU_TYPE_FIXED_SIZE_BINARY, 1,
                          DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 7);
    return DFTU_OK;
}

dftu_status nested(dftu_ctx*, void*, dftu_schema* s) {
    int32_t list_idx = dftu_schema_add_field(
        s, "tags", DFTU_TYPE_LIST, 1, DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 0);
    dftu_schema_add_child_field(s, list_idx, "item", DFTU_TYPE_INT64, 1,
                                DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 0);

    int32_t struct_idx =
        dftu_schema_add_field(s, "point", DFTU_TYPE_STRUCT, 1,
                              DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 0);
    dftu_schema_add_child_field(s, struct_idx, "x", DFTU_TYPE_INT64, 1,
                                DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 0);
    dftu_schema_add_child_field(s, struct_idx, "y", DFTU_TYPE_STRING, 1,
                                DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 0);
    return DFTU_OK;
}

// A List holds exactly one element type, so the second child is refused at
// the call. Records both return values so the test can assert the refusal
// happens there rather than being discovered when the schema is converted.
std::int32_t g_second_child_result = 0;

dftu_status malformed_list(dftu_ctx*, void*, dftu_schema* s) {
    int32_t list_idx = dftu_schema_add_field(
        s, "tags", DFTU_TYPE_LIST, 1, DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 0);
    dftu_schema_add_child_field(s, list_idx, "a", DFTU_TYPE_INT64, 1,
                                DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 0);
    g_second_child_result =
        dftu_schema_add_child_field(s, list_idx, "b", DFTU_TYPE_INT64, 1,
                                    DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 0);
    return DFTU_OK;
}

}  // namespace

TEST_SUITE("provider schema types") {
    TEST_CASE("plain scalar types are reported exactly") {
        Fixture fx{{"id", "val"}, plain_scalars};
        dataframe::Schema s = make_fixture(fx)->schema();
        REQUIRE(s.fields.size() == 2);
        CHECK(s.fields[0].name == "id");
        CHECK(s.fields[0].type.id == dataframe::TypeId::Int64);
        CHECK(s.fields[0].nullable == false);
        CHECK(s.fields[1].name == "val");
        CHECK(s.fields[1].type.id == dataframe::TypeId::Float64);
        CHECK(s.fields[1].nullable == true);
    }

    TEST_CASE(
        "parameterized types keep their parameters, not just the TypeId") {
        Fixture fx{{"ts", "amount", "digest"}, parameterized};
        dataframe::Schema s = make_fixture(fx)->schema();
        REQUIRE(s.fields.size() == 3);

        const dataframe::DataType& ts = s.fields[0].type;
        CHECK(ts.id == dataframe::TypeId::Timestamp);
        CHECK(ts.time_unit() == dataframe::TimeUnit::Nano);
        CHECK(ts.timezone() == "UTC");

        const dataframe::DataType& amount = s.fields[1].type;
        CHECK(amount.id == dataframe::TypeId::Decimal128);
        CHECK(amount.decimal_precision() == 38);
        CHECK(amount.decimal_scale() == 9);

        const dataframe::DataType& digest = s.fields[2].type;
        CHECK(digest.id == dataframe::TypeId::FixedSizeBinary);
        CHECK(digest.fixed_size() == 7);
    }

    TEST_CASE("nested List and Struct types are expressible") {
        Fixture fx{{"tags", "point"}, nested};
        dataframe::Schema s = make_fixture(fx)->schema();
        REQUIRE(s.fields.size() == 2);

        const dataframe::DataType& tags = s.fields[0].type;
        CHECK(tags.id == dataframe::TypeId::List);
        REQUIRE(tags.fields.size() == 1);
        CHECK(tags.fields[0].type.id == dataframe::TypeId::Int64);

        const dataframe::DataType& point = s.fields[1].type;
        CHECK(point.id == dataframe::TypeId::Struct);
        REQUIRE(point.fields.size() == 2);
        CHECK(point.fields[0].name == "x");
        CHECK(point.fields[0].type.id == dataframe::TypeId::Int64);
        CHECK(point.fields[1].name == "y");
        CHECK(point.fields[1].type.id == dataframe::TypeId::String);
    }

    TEST_CASE("a second element under a List is refused at the call") {
        g_second_child_result = 0;
        Fixture fx{{"tags"}, malformed_list};
        dataframe::Schema s = make_fixture(fx)->schema();
        // Refused where it happens, so the builder never holds a shape that
        // would have to be thrown away later.
        CHECK(g_second_child_result == -1);
        REQUIRE(s.fields.size() == 1);
        CHECK(s.fields[0].type.id == dataframe::TypeId::List);
        REQUIRE(s.fields[0].type.fields.size() == 1);
        CHECK(s.fields[0].type.fields[0].type.id == dataframe::TypeId::Int64);
    }

    TEST_CASE("a provider declaring nothing still works and reports Unknown") {
        Fixture fx{{"id", "val"}, nullptr};
        dataframe::Schema s = make_fixture(fx)->schema();
        REQUIRE(s.fields.size() == 2);
        CHECK(s.fields[0].name == "id");
        CHECK(s.fields[0].type.id == dataframe::TypeId::Unknown);
        CHECK(s.fields[1].name == "val");
        CHECK(s.fields[1].type.id == dataframe::TypeId::Unknown);
    }

    TEST_CASE(
        "LazyFrame::output_schema is typed end to end over a provider plan") {
        Fixture fx{
            {"id", "val", "ts"},
            [](dftu_ctx*, void*, dftu_schema* s) -> dftu_status {
                dftu_schema_add_field(s, "id", DFTU_TYPE_INT64, 0,
                                      DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 0);
                dftu_schema_add_field(s, "val", DFTU_TYPE_FLOAT64, 1,
                                      DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 0);
                dftu_schema_add_field(s, "ts", DFTU_TYPE_TIMESTAMP, 1,
                                      DFTU_TIME_UNIT_MILLI, "UTC", 0, 0, 0);
                return DFTU_OK;
            }};
        dataframe::LazyFrame lf =
            dataframe::LazyFrame::scan(make_fixture(fx))
                .filter(dataframe::col(0) > std::int64_t{0})
                .select({"val", "ts"});
        dataframe::Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 2);
        CHECK(s.fields[0].name == "val");
        CHECK(s.fields[0].type.id == dataframe::TypeId::Float64);
        CHECK(s.fields[1].name == "ts");
        CHECK(s.fields[1].type.id == dataframe::TypeId::Timestamp);
        CHECK(s.fields[1].type.time_unit() == dataframe::TimeUnit::Milli);
        CHECK(s.fields[1].type.timezone() == "UTC");
    }

    TEST_CASE("a frame whose column type differs from the declared one fails") {
        // The fixture's morsel holds Int64 columns; "val" is declared float64.
        Fixture fx{{"id", "val"}, plain_scalars};
        const auto collected =
            dftracer::utils::default_runtime()
                .submit(dataframe::LazyFrame::scan(make_fixture(fx)).collect())
                .get();
        dftu_utils_test::expect_error(collected,
                                      common::ErrorCode::InvalidArgument);
        const dstd::String msg(collected.error().c_str());
        CHECK(msg.find("column val") != dstd::String::npos);
        CHECK(msg.find("declared float64") != dstd::String::npos);
    }
}
