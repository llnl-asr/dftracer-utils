// dftu_source_vt::schema_types lets a provider declare each column's type
// (dftu_schema_add_field/add_child_field), which ProviderSource::schema()
// reports instead of the TypeId::Unknown every column got before. Exercises
// plain scalars, parameterized types (Timestamp/Decimal128/FixedSizeBinary,
// where the parameters - not just the TypeId - must survive), nested types
// (List/Struct), the additive fallback when a provider declares nothing, and
// LazyFrame::output_schema() end to end over a provider-rooted plan.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/provider_source.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using dftracer::utils::dataframe::col;
using dftracer::utils::dataframe::DataType;
using dftracer::utils::dataframe::LazyFrame;
using dftracer::utils::dataframe::make_provider_source;
using dftracer::utils::dataframe::Schema;
using dftracer::utils::dataframe::Source;
using dftracer::utils::dataframe::TimeUnit;
using dftracer::utils::dataframe::TypeId;

namespace {

dftu_dataframe* empty_frame(const std::vector<std::string>& names) {
    std::vector<const char*> cnames;
    std::vector<dftu_series*> cols;
    for (const std::string& n : names) {
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

dftu_task* cursor_next(void* self, std::int64_t, dftu_result_frame* out) {
    auto* cs = static_cast<CursorState*>(self);
    out->ok = 1;
    if (cs->done) {
        out->u.value = nullptr;
    } else {
        out->u.value = cs->frame;
        cs->frame = nullptr;
        cs->done = true;
    }
    return nullptr;
}

void cursor_destroy(void* self) {
    auto* cs = static_cast<CursorState*>(self);
    if (cs->frame) dftu_dataframe_free(cs->frame);
    delete cs;
}

const dftu_cursor_vt CURSOR_VT = {cursor_next, cursor_destroy, nullptr, nullptr,
                                  nullptr};

// A source over the given names, whose scan() always returns one empty
// morsel. `schema_types` is the vtable's optional type-declaration callback;
// pass nullptr for a provider that declares nothing.
struct Fixture {
    std::vector<std::string> names;
    void (*schema_types)(void*, dftu_schema*);
};

int32_t fixture_schema(void* self, const char* const** out_names) {
    auto* fx = static_cast<Fixture*>(self);
    static thread_local std::vector<const char*> storage;
    storage.clear();
    for (const std::string& n : fx->names) storage.push_back(n.c_str());
    *out_names = storage.data();
    return static_cast<int32_t>(storage.size());
}

void* fixture_scan(void* self, const dftu_scan_request*, int32_t*,
                   void** out_cursor_self, const dftu_cursor_vt** out_vt) {
    auto* fx = static_cast<Fixture*>(self);
    auto* cs = new CursorState{empty_frame(fx->names)};
    *out_cursor_self = cs;
    *out_vt = &CURSOR_VT;
    return cs;
}

void fixture_destroy(void*) {}

std::shared_ptr<Source> make_fixture(Fixture& fx) {
    dftu_source_vt vt{};
    vt.schema = fixture_schema;
    vt.scan = fixture_scan;
    vt.destroy = fixture_destroy;
    vt.schema_types = fx.schema_types;
    return make_provider_source(vt, &fx);
}

void plain_scalars(void*, dftu_schema* s) {
    dftu_schema_add_field(s, "id", DFTU_TYPE_INT64, 0, DFTU_TIME_UNIT_MICRO,
                          nullptr, 0, 0, 0);
    dftu_schema_add_field(s, "val", DFTU_TYPE_FLOAT64, 1, DFTU_TIME_UNIT_MICRO,
                          nullptr, 0, 0, 0);
}

void parameterized(void*, dftu_schema* s) {
    dftu_schema_add_field(s, "ts", DFTU_TYPE_TIMESTAMP, 1, DFTU_TIME_UNIT_NANO,
                          "UTC", 0, 0, 0);
    dftu_schema_add_field(s, "amount", DFTU_TYPE_DECIMAL128, 1,
                          DFTU_TIME_UNIT_MICRO, nullptr, 38, 9, 0);
    dftu_schema_add_field(s, "digest", DFTU_TYPE_FIXED_SIZE_BINARY, 1,
                          DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 7);
}

void nested(void*, dftu_schema* s) {
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
}

// A List holds exactly one element type, so the second child is refused at
// the call. Records both return values so the test can assert the refusal
// happens there rather than being discovered when the schema is converted.
std::int32_t g_second_child_result = 0;

void malformed_list(void*, dftu_schema* s) {
    int32_t list_idx = dftu_schema_add_field(
        s, "tags", DFTU_TYPE_LIST, 1, DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 0);
    dftu_schema_add_child_field(s, list_idx, "a", DFTU_TYPE_INT64, 1,
                                DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 0);
    g_second_child_result =
        dftu_schema_add_child_field(s, list_idx, "b", DFTU_TYPE_INT64, 1,
                                    DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 0);
}

}  // namespace

TEST_SUITE("provider schema types") {
    TEST_CASE("plain scalar types are reported exactly") {
        Fixture fx{{"id", "val"}, plain_scalars};
        Schema s = make_fixture(fx)->schema();
        REQUIRE(s.fields.size() == 2);
        CHECK(s.fields[0].name == "id");
        CHECK(s.fields[0].type.id == TypeId::Int64);
        CHECK(s.fields[0].nullable == false);
        CHECK(s.fields[1].name == "val");
        CHECK(s.fields[1].type.id == TypeId::Float64);
        CHECK(s.fields[1].nullable == true);
    }

    TEST_CASE(
        "parameterized types keep their parameters, not just the TypeId") {
        Fixture fx{{"ts", "amount", "digest"}, parameterized};
        Schema s = make_fixture(fx)->schema();
        REQUIRE(s.fields.size() == 3);

        const DataType& ts = s.fields[0].type;
        CHECK(ts.id == TypeId::Timestamp);
        CHECK(ts.time_unit == TimeUnit::Nano);
        CHECK(ts.timezone == "UTC");

        const DataType& amount = s.fields[1].type;
        CHECK(amount.id == TypeId::Decimal128);
        CHECK(amount.decimal_precision == 38);
        CHECK(amount.decimal_scale == 9);

        const DataType& digest = s.fields[2].type;
        CHECK(digest.id == TypeId::FixedSizeBinary);
        CHECK(digest.fixed_size == 7);
    }

    TEST_CASE("nested List and Struct types are expressible") {
        Fixture fx{{"tags", "point"}, nested};
        Schema s = make_fixture(fx)->schema();
        REQUIRE(s.fields.size() == 2);

        const DataType& tags = s.fields[0].type;
        CHECK(tags.id == TypeId::List);
        REQUIRE(tags.fields.size() == 1);
        CHECK(tags.fields[0].type.id == TypeId::Int64);

        const DataType& point = s.fields[1].type;
        CHECK(point.id == TypeId::Struct);
        REQUIRE(point.fields.size() == 2);
        CHECK(point.fields[0].name == "x");
        CHECK(point.fields[0].type.id == TypeId::Int64);
        CHECK(point.fields[1].name == "y");
        CHECK(point.fields[1].type.id == TypeId::String);
    }

    TEST_CASE("a second element under a List is refused at the call") {
        g_second_child_result = 0;
        Fixture fx{{"tags"}, malformed_list};
        Schema s = make_fixture(fx)->schema();
        // Refused where it happens, so the builder never holds a shape that
        // would have to be thrown away later.
        CHECK(g_second_child_result == -1);
        REQUIRE(s.fields.size() == 1);
        CHECK(s.fields[0].type.id == TypeId::List);
        REQUIRE(s.fields[0].type.fields.size() == 1);
        CHECK(s.fields[0].type.fields[0].type.id == TypeId::Int64);
    }

    TEST_CASE("a provider declaring nothing still works and reports Unknown") {
        Fixture fx{{"id", "val"}, nullptr};
        Schema s = make_fixture(fx)->schema();
        REQUIRE(s.fields.size() == 2);
        CHECK(s.fields[0].name == "id");
        CHECK(s.fields[0].type.id == TypeId::Unknown);
        CHECK(s.fields[1].name == "val");
        CHECK(s.fields[1].type.id == TypeId::Unknown);
    }

    TEST_CASE(
        "LazyFrame::output_schema is typed end to end over a provider plan") {
        Fixture fx{
            {"id", "val", "ts"}, [](void*, dftu_schema* s) {
                dftu_schema_add_field(s, "id", DFTU_TYPE_INT64, 0,
                                      DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 0);
                dftu_schema_add_field(s, "val", DFTU_TYPE_FLOAT64, 1,
                                      DFTU_TIME_UNIT_MICRO, nullptr, 0, 0, 0);
                dftu_schema_add_field(s, "ts", DFTU_TYPE_TIMESTAMP, 1,
                                      DFTU_TIME_UNIT_MILLI, "UTC", 0, 0, 0);
            }};
        LazyFrame lf = LazyFrame::scan(make_fixture(fx))
                           .filter(col(0) > std::int64_t{0})
                           .select({"val", "ts"});
        Schema s = lf.output_schema();
        REQUIRE(s.fields.size() == 2);
        CHECK(s.fields[0].name == "val");
        CHECK(s.fields[0].type.id == TypeId::Float64);
        CHECK(s.fields[1].name == "ts");
        CHECK(s.fields[1].type.id == TypeId::Timestamp);
        CHECK(s.fields[1].type.time_unit == TimeUnit::Milli);
        CHECK(s.fields[1].type.timezone == "UTC");
    }
}
