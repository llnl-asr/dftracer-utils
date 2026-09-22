#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/common/arrow/array_view.h>
#include <dftracer/utils/utilities/common/arrow/arrow.h>
#include <doctest/doctest.h>
#include <nanoarrow/nanoarrow.h>

#include <string>
#include <vector>

using namespace dftracer::utils::utilities::common::arrow;

static std::string child_schema_name(const ArrowExportResult& r, int64_t i) {
    return r.get_schema()->children[i]->name;
}

static std::string child_schema_format(const ArrowExportResult& r, int64_t i) {
    return r.get_schema()->children[i]->format;
}

static int64_t child_null_count(const ArrowExportResult& r, int64_t i) {
    return r.get_array()->children[i]->null_count;
}

TEST_CASE("RecordBatchBuilder - Static schema mode") {
    SUBCASE("builds batch with int64, double, string columns") {
        RecordBatchBuilder b;
        b.declare_schema({{"id", ColumnType::INT64},
                          {"value", ColumnType::DOUBLE},
                          {"name", ColumnType::STRING}});

        std::string s0 = "alice", s1 = "bob", s2 = "carol";

        b.append_int64(0, 1);
        b.append_double(1, 1.1);
        b.append_string(2, s0);
        b.end_row();

        b.append_int64(0, 2);
        b.append_double(1, 2.2);
        b.append_string(2, s1);
        b.end_row();

        b.append_int64(0, 3);
        b.append_double(1, 3.3);
        b.append_string(2, s2);
        b.end_row();

        auto result = b.finish();

        REQUIRE(result.valid());
        CHECK(result.num_rows() == 3);
        CHECK(result.num_columns() == 3);

        CHECK(child_schema_name(result, 0) == "id");
        CHECK(child_schema_name(result, 1) == "value");
        CHECK(child_schema_name(result, 2) == "name");

        // nanoarrow format strings: "l"=int64, "g"=float64, "u"=utf8 string
        CHECK(child_schema_format(result, 0) == std::string("l"));
        CHECK(child_schema_format(result, 1) == std::string("g"));
        CHECK(child_schema_format(result, 2) == std::string("u"));
    }

    SUBCASE("reserve and reset cycle for streaming") {
        RecordBatchBuilder b;
        b.declare_schema(
            {{"ts", ColumnType::INT64}, {"dur", ColumnType::DOUBLE}});
        b.reserve(100);

        for (int i = 0; i < 5; ++i) {
            b.append_int64(0, i);
            b.append_double(1, static_cast<double>(i) * 0.5);
            b.end_row();
        }

        auto first = b.finish();
        REQUIRE(first.valid());
        CHECK(first.num_rows() == 5);

        b.reset(true);  // keep schema

        for (int i = 0; i < 3; ++i) {
            b.append_int64(0, i + 100);
            b.append_double(1, static_cast<double>(i) * 1.5);
            b.end_row();
        }

        auto second = b.finish();
        REQUIRE(second.valid());
        CHECK(second.num_rows() == 3);
        CHECK(second.num_columns() == 2);
    }
}

TEST_CASE("RecordBatchBuilder - Dynamic schema mode") {
    SUBCASE("discovers columns from data") {
        RecordBatchBuilder b;

        size_t x = b.add_or_get_column("x", ColumnType::INT64);
        CHECK(x == 0);

        b.append_int64(x, 42);
        b.end_row();

        size_t y = b.add_or_get_column("y", ColumnType::STRING);
        CHECK(y == 1);

        std::string hello = "hello";
        b.append_int64(x, 43);
        b.append_string(y, hello);
        b.end_row();

        auto result = b.finish();
        REQUIRE(result.valid());
        CHECK(result.num_rows() == 2);
        CHECK(result.num_columns() == 2);

        CHECK(child_schema_name(result, 0) == "x");
        CHECK(child_schema_name(result, 1) == "y");
    }

    SUBCASE("backfills nulls for missing columns") {
        RecordBatchBuilder b;

        size_t a = b.add_or_get_column("a", ColumnType::INT64);
        b.append_int64(a, 1);
        b.end_row();  // row 0: a=1, b not yet known

        size_t bv = b.add_or_get_column("b", ColumnType::DOUBLE);
        b.append_int64(a, 2);
        b.append_double(bv, 3.14);
        b.end_row();  // row 1: a=2, b=3.14

        auto result = b.finish();
        REQUIRE(result.valid());
        CHECK(result.num_rows() == 2);
        CHECK(result.num_columns() == 2);

        // Column "b" must have a null in row 0.
        CHECK(child_null_count(result, 1) > 0);
    }
}

TEST_CASE("RecordBatchBuilder - Null handling") {
    RecordBatchBuilder b;
    b.declare_schema({{"v", ColumnType::INT64}});

    b.append_int64(0, 10);
    b.end_row();

    b.append_null(0);
    b.end_row();

    b.append_int64(0, 30);
    b.end_row();

    auto result = b.finish();
    REQUIRE(result.valid());
    CHECK(result.num_rows() == 3);
    CHECK(child_null_count(result, 0) == 1);
}

TEST_CASE("RecordBatchBuilder - append_null under a locked schema") {
    // append_null must gate first-touch marking on schema_locked_ like every
    // other append_*; a locked schema uses explicit backfill, not first-touch
    // row detection. This exercises that mixed real/null appends after
    // lock_schema() still produce a well-formed batch.
    RecordBatchBuilder b;
    b.add_or_get_column("a", ColumnType::INT64);
    b.add_or_get_column("b", ColumnType::INT64);
    b.lock_schema();
    CHECK(b.is_schema_locked());

    b.append_int64(0, 1);
    b.append_null(1);
    b.end_row();

    b.append_int64(0, 2);
    b.append_int64(1, 3);
    b.end_row();

    auto result = b.finish();
    REQUIRE(result.valid());
    CHECK(result.num_rows() == 2);
    CHECK(result.num_columns() == 2);
    CHECK(child_null_count(result, 0) == 0);
    CHECK(child_null_count(result, 1) == 1);
}

TEST_CASE("RecordBatchBuilder - Empty batch") {
    RecordBatchBuilder b;
    b.declare_schema({{"a", ColumnType::INT64}, {"b", ColumnType::STRING}});

    auto result = b.finish();
    REQUIRE(result.valid());
    CHECK(result.num_rows() == 0);
    CHECK(result.num_columns() == 2);
}

TEST_CASE("RecordBatchBuilder - Bool column") {
    RecordBatchBuilder b;
    b.declare_schema({{"flag", ColumnType::BOOL}});

    b.append_bool(0, true);
    b.end_row();
    b.append_bool(0, false);
    b.end_row();
    b.append_bool(0, true);
    b.end_row();

    auto result = b.finish();
    REQUIRE(result.valid());
    CHECK(result.num_rows() == 3);
    CHECK(result.num_columns() == 1);
}

TEST_CASE("RecordBatchBuilder - HIST list<struct> column") {
    RecordBatchBuilder b;
    b.declare_schema({{"name", ColumnType::STRING}, {"h", ColumnType::HIST}});

    b.append_string(0, "read");
    b.append_hist(1, {{0.0, 1.0, 3}, {1.0, 2.0, 5}});
    b.end_row();

    b.append_string(0, "write");
    b.append_hist(1, {});  // empty histogram
    b.end_row();

    auto result = b.finish();
    REQUIRE(result.valid());
    CHECK(result.num_rows() == 2);
    CHECK(result.num_columns() == 2);
    // list<item: struct<lo,hi,count>>: format "+l" with a struct child.
    CHECK(child_schema_format(result, 1) == std::string("+l"));
    CHECK(std::string(result.get_schema()->children[1]->children[0]->format) ==
          std::string("+s"));
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW
