// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/utilities/arrow/array_view.h>
#include <dftracer/utils/utilities/arrow/arrow.h>
#include <doctest/doctest.h>
#include <nanoarrow/nanoarrow.h>

#include <limits>

using namespace dftracer::utils;

using namespace dftracer::utils::utilities::arrow;

static dstd::String child_schema_name(const ArrowExportResult& r, int64_t i) {
    return r.get_schema()->children[i]->name;
}

static dstd::String child_schema_format(const ArrowExportResult& r, int64_t i) {
    return r.get_schema()->children[i]->format;
}

static int64_t child_null_count(const ArrowExportResult& r, int64_t i) {
    return r.get_array()->children[i]->null_count;
}

TEST_CASE("RecordBatchBuilder - Static schema mode") {
    SUBCASE("builds batch with int64, double, string columns") {
        RecordBatchBuilder b;
        b.declare_schema({{"id", ColumnType::Int64},
                          {"value", ColumnType::Double},
                          {"name", ColumnType::String}});

        dstd::String s0 = "alice", s1 = "bob", s2 = "carol";

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
        CHECK(child_schema_format(result, 0) == dstd::String("l"));
        CHECK(child_schema_format(result, 1) == dstd::String("g"));
        CHECK(child_schema_format(result, 2) == dstd::String("u"));
    }

    SUBCASE("reserve and reset cycle for streaming") {
        RecordBatchBuilder b;
        b.declare_schema(
            {{"ts", ColumnType::Int64}, {"dur", ColumnType::Double}});
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

        size_t x = b.add_or_get_column("x", ColumnType::Int64);
        CHECK(x == 0);

        b.append_int64(x, 42);
        b.end_row();

        size_t y = b.add_or_get_column("y", ColumnType::String);
        CHECK(y == 1);

        dstd::String hello = "hello";
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

        size_t a = b.add_or_get_column("a", ColumnType::Int64);
        b.append_int64(a, 1);
        b.end_row();  // row 0: a=1, b not yet known

        size_t bv = b.add_or_get_column("b", ColumnType::Double);
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
    b.declare_schema({{"v", ColumnType::Int64}});

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
    b.add_or_get_column("a", ColumnType::Int64);
    b.add_or_get_column("b", ColumnType::Int64);
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
    b.declare_schema({{"a", ColumnType::Int64}, {"b", ColumnType::String}});

    auto result = b.finish();
    REQUIRE(result.valid());
    CHECK(result.num_rows() == 0);
    CHECK(result.num_columns() == 2);
}

TEST_CASE("RecordBatchBuilder - Bool column") {
    RecordBatchBuilder b;
    b.declare_schema({{"flag", ColumnType::Bool}});

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
    b.declare_schema({{"name", ColumnType::String}, {"h", ColumnType::Hist}});

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
    CHECK(child_schema_format(result, 1) == dstd::String("+l"));
    CHECK(dstd::String(result.get_schema()->children[1]->children[0]->format) ==
          dstd::String("+s"));
}

namespace {

// The cells of column `col` of a finished batch, read through nanoarrow.
struct Cells {
    ArrowArrayView view;
    explicit Cells(ArrowExportResult& r) {
        REQUIRE(init_array_view(view, r.get_schema(), r.get_array()) ==
                NANOARROW_OK);
    }
    ~Cells() { ArrowArrayViewReset(&view); }
    Cells(const Cells&) = delete;
    Cells& operator=(const Cells&) = delete;
    bool is_null(int col, std::int64_t row) const {
        return ArrowArrayViewIsNull(view.children[col], row);
    }
    std::int64_t i64(int col, std::int64_t row) const {
        return ArrowArrayViewGetIntUnsafe(view.children[col], row);
    }
    double f64(int col, std::int64_t row) const {
        return ArrowArrayViewGetDoubleUnsafe(view.children[col], row);
    }
    dstd::String str(int col, std::int64_t row) const {
        const ArrowStringView s =
            ArrowArrayViewGetStringUnsafe(view.children[col], row);
        return dstd::String(s.data, static_cast<std::size_t>(s.size_bytes));
    }
};

}  // namespace

TEST_CASE("RecordBatchBuilder - mixed numbers promote the column to Double") {
    SUBCASE("int64 then double") {
        RecordBatchBuilder b;
        auto x = b.add_or_get_column("x", ColumnType::Int64);
        b.append_int64(x, 1);
        b.end_row();
        x = b.add_or_get_column("x", ColumnType::Double);
        b.append_double(x, 1.5);
        b.end_row();
        auto result = b.finish();
        REQUIRE(result.valid());
        CHECK(child_schema_format(result, 0) == dstd::String("g"));
        Cells c(result);
        CHECK(c.f64(0, 0) == 1.0);
        CHECK(c.f64(0, 1) == 1.5);
    }
    SUBCASE("double then int64") {
        RecordBatchBuilder b;
        auto x = b.add_or_get_column("x", ColumnType::Double);
        b.append_double(x, 1.5);
        b.end_row();
        x = b.add_or_get_column("x", ColumnType::Int64);
        b.append_int64(x, 7);
        b.end_row();
        auto result = b.finish();
        REQUIRE(result.valid());
        CHECK(child_schema_format(result, 0) == dstd::String("g"));
        Cells c(result);
        CHECK(c.f64(0, 0) == 1.5);
        CHECK(c.f64(0, 1) == 7.0);
    }
    SUBCASE("int64 with uint64 in both orders") {
        for (const bool signed_first : {true, false}) {
            RecordBatchBuilder b;
            auto x = b.add_or_get_column(
                "x", signed_first ? ColumnType::Int64 : ColumnType::Uint64);
            if (signed_first)
                b.append_int64(x, -3);
            else
                b.append_uint64(x, 18446744073709551615ULL);
            b.end_row();
            x = b.add_or_get_column(
                "x", signed_first ? ColumnType::Uint64 : ColumnType::Int64);
            if (signed_first)
                b.append_uint64(x, 18446744073709551615ULL);
            else
                b.append_int64(x, -3);
            b.end_row();
            auto result = b.finish();
            REQUIRE(result.valid());
            CHECK(child_schema_format(result, 0) == dstd::String("g"));
            Cells c(result);
            const double big = 18446744073709551615.0;
            CHECK(c.f64(0, 0) == (signed_first ? -3.0 : big));
            CHECK(c.f64(0, 1) == (signed_first ? big : -3.0));
        }
    }
    SUBCASE("a null stays a null and rows added before the column are null") {
        RecordBatchBuilder b;
        auto y = b.add_or_get_column("y", ColumnType::Int64);
        b.append_int64(y, 1);
        b.end_row();
        auto x = b.add_or_get_column("x", ColumnType::Int64);
        b.append_int64(y, 2);
        b.append_int64(x, 5);
        b.end_row();
        b.append_int64(y, 3);
        b.append_null(x);
        b.end_row();
        x = b.add_or_get_column("x", ColumnType::Double);
        b.append_int64(y, 4);
        b.append_double(x, 2.5);
        b.end_row();
        auto result = b.finish();
        REQUIRE(result.valid());
        Cells c(result);
        CHECK(c.is_null(1, 0));
        CHECK(c.f64(1, 1) == 5.0);
        CHECK(c.is_null(1, 2));
        CHECK(c.f64(1, 3) == 2.5);
    }
    SUBCASE("a stale index keeps working after the column is promoted") {
        RecordBatchBuilder b;
        const auto x = b.add_or_get_column("x", ColumnType::Int64);
        b.append_int64(x, 1);
        b.end_row();
        b.add_or_get_column("x", ColumnType::Double);
        b.append_int64(x, 2);
        b.end_row();
        b.append_uint64(x, 3);
        b.end_row();
        auto result = b.finish();
        REQUIRE(result.valid());
        Cells c(result);
        CHECK(c.f64(0, 0) == 1.0);
        CHECK(c.f64(0, 1) == 2.0);
        CHECK(c.f64(0, 2) == 3.0);
    }
}

TEST_CASE(
    "RecordBatchBuilder - a string, bool or nested value makes it String") {
    SUBCASE("number then string") {
        RecordBatchBuilder b;
        auto x = b.add_or_get_column("x", ColumnType::Int64);
        b.append_int64(x, 42);
        b.end_row();
        b.append_null(x);
        b.end_row();
        x = b.add_or_get_column("x", ColumnType::String);
        b.append_string(x, "abc");
        b.end_row();
        auto result = b.finish();
        REQUIRE(result.valid());
        CHECK(child_schema_format(result, 0) == dstd::String("u"));
        Cells c(result);
        CHECK(c.str(0, 0) == "42");
        CHECK(c.is_null(0, 1));
        CHECK(c.str(0, 2) == "abc");
    }
    SUBCASE("string then each scalar") {
        RecordBatchBuilder b;
        auto x = b.add_or_get_column("x", ColumnType::String);
        b.append_string(x, "abc");
        b.end_row();
        x = b.add_or_get_column("x", ColumnType::Int64);
        b.append_int64(x, -7);
        b.end_row();
        x = b.add_or_get_column("x", ColumnType::Uint64);
        b.append_uint64(x, 18446744073709551615ULL);
        b.end_row();
        x = b.add_or_get_column("x", ColumnType::Double);
        b.append_double(x, 1.5);
        b.end_row();
        b.append_double(x, 2.0);
        b.end_row();
        x = b.add_or_get_column("x", ColumnType::Bool);
        b.append_bool(x, true);
        b.end_row();
        auto result = b.finish();
        REQUIRE(result.valid());
        Cells c(result);
        CHECK(c.str(0, 0) == "abc");
        CHECK(c.str(0, 1) == "-7");
        CHECK(c.str(0, 2) == "18446744073709551615");
        CHECK(c.str(0, 3) == "1.5");
        CHECK(c.str(0, 4) == "2.0");
        CHECK(c.str(0, 5) == "true");
    }
    SUBCASE("each scalar then string converts the earlier rows") {
        const ColumnType firsts[] = {ColumnType::Int64, ColumnType::Uint64,
                                     ColumnType::Double, ColumnType::Bool};
        const char* const texts[] = {"-4", "9", "0.25", "false"};
        for (int k = 0; k < 4; ++k) {
            RecordBatchBuilder b;
            auto x = b.add_or_get_column("x", firsts[k]);
            if (k == 0) b.append_int64(x, -4);
            if (k == 1) b.append_uint64(x, 9);
            if (k == 2) b.append_double(x, 0.25);
            if (k == 3) b.append_bool(x, false);
            b.end_row();
            x = b.add_or_get_column("x", ColumnType::String);
            b.append_string(x, "s");
            b.end_row();
            auto result = b.finish();
            REQUIRE(result.valid());
            Cells c(result);
            CHECK(c.str(0, 0) == texts[k]);
            CHECK(c.str(0, 1) == "s");
        }
    }
    SUBCASE("bool with a number, both orders") {
        for (const bool bool_first : {true, false}) {
            RecordBatchBuilder b;
            auto x = b.add_or_get_column(
                "x", bool_first ? ColumnType::Bool : ColumnType::Int64);
            if (bool_first)
                b.append_bool(x, true);
            else
                b.append_int64(x, 3);
            b.end_row();
            x = b.add_or_get_column(
                "x", bool_first ? ColumnType::Int64 : ColumnType::Bool);
            if (bool_first)
                b.append_int64(x, 3);
            else
                b.append_bool(x, true);
            b.end_row();
            auto result = b.finish();
            REQUIRE(result.valid());
            Cells c(result);
            CHECK(c.str(0, bool_first ? 0 : 1) == "true");
            CHECK(c.str(0, bool_first ? 1 : 0) == "3");
        }
    }
    SUBCASE("a dict string column with a number") {
        RecordBatchBuilder b;
        auto x = b.add_or_get_column("x", ColumnType::DictString);
        b.append_dict_string(x, "a");
        b.end_row();
        b.append_dict_string(x, "b");
        b.end_row();
        x = b.add_or_get_column("x", ColumnType::Int64);
        b.append_int64(x, 5);
        b.end_row();
        auto result = b.finish();
        REQUIRE(result.valid());
        CHECK(child_schema_format(result, 0) == dstd::String("u"));
        Cells c(result);
        CHECK(c.str(0, 0) == "a");
        CHECK(c.str(0, 1) == "b");
        CHECK(c.str(0, 2) == "5");
    }
}

TEST_CASE(
    "RecordBatchBuilder - a declared type is kept, a locked one promotes") {
    SUBCASE("a number in a locked Double column converts") {
        RecordBatchBuilder b;
        const auto x = b.add_or_get_column("x", ColumnType::Double);
        b.lock_schema();
        b.append_int64(x, 4);
        b.end_row();
        b.append_double(x, 0.5);
        b.end_row();
        auto result = b.finish();
        REQUIRE(result.valid());
        CHECK(child_schema_format(result, 0) == dstd::String("g"));
        Cells c(result);
        CHECK(c.f64(0, 0) == 4.0);
        CHECK(c.f64(0, 1) == 0.5);
    }
    SUBCASE("a locked Int64 column promotes, and the next batch keeps it") {
        RecordBatchBuilder b;
        auto x = b.add_or_get_column("x", ColumnType::Int64);
        b.append_int64(x, 8);
        b.end_row();
        b.lock_schema();
        x = b.add_or_get_column("x", ColumnType::Double);
        b.append_double(x, 1.5);
        b.end_row();
        x = b.add_or_get_column("x", ColumnType::String);
        b.append_string(x, "abc");
        b.end_row();
        x = b.add_or_get_column("x", ColumnType::Bool);
        b.append_bool(x, true);
        b.end_row();
        {
            auto result = b.finish();
            REQUIRE(result.valid());
            CHECK(child_schema_format(result, 0) == dstd::String("u"));
            Cells c(result);
            CHECK(c.str(0, 0) == "8.0");
            CHECK(c.str(0, 1) == "1.5");
            CHECK(c.str(0, 2) == "abc");
            CHECK(c.str(0, 3) == "true");
        }
        b.reset(true);
        x = b.add_or_get_column("x", ColumnType::Int64);
        b.append_int64(x, 5);
        b.end_row();
        auto next = b.finish();
        REQUIRE(next.valid());
        CHECK(child_schema_format(next, 0) == dstd::String("u"));
        Cells c(next);
        CHECK(c.str(0, 0) == "5");
    }
    SUBCASE(
        "a locked Int64 column promotes to Double, and the next batch keeps "
        "it") {
        RecordBatchBuilder b;
        auto x = b.add_or_get_column("x", ColumnType::Int64);
        b.append_int64(x, 8);
        b.end_row();
        b.lock_schema();
        x = b.add_or_get_column("x", ColumnType::Double);
        b.append_double(x, 1.5);
        b.end_row();
        {
            auto result = b.finish();
            REQUIRE(result.valid());
            CHECK(child_schema_format(result, 0) == dstd::String("g"));
            Cells c(result);
            CHECK(c.f64(0, 0) == 8.0);
            CHECK(c.f64(0, 1) == 1.5);
        }
        b.reset(true);
        x = b.add_or_get_column("x", ColumnType::Int64);
        b.append_int64(x, 5);
        b.end_row();
        auto next = b.finish();
        REQUIRE(next.valid());
        CHECK(child_schema_format(next, 0) == dstd::String("g"));
        Cells c(next);
        CHECK(c.f64(0, 0) == 5.0);
    }
    SUBCASE("a value a declared Int64 column cannot hold is a null") {
        RecordBatchBuilder b;
        b.declare_schema({{"x", ColumnType::Int64}});
        b.append_double(0, 1.5);
        b.end_row();
        b.append_string(0, "abc");
        b.end_row();
        b.append_bool(0, true);
        b.end_row();
        b.append_int64(0, 8);
        b.end_row();
        auto result = b.finish();
        REQUIRE(result.valid());
        CHECK(child_schema_format(result, 0) == dstd::String("l"));
        Cells c(result);
        CHECK(c.is_null(0, 0));
        CHECK(c.is_null(0, 1));
        CHECK(c.is_null(0, 2));
        CHECK(c.i64(0, 3) == 8);
    }
    SUBCASE("a declared String column takes a number as text") {
        RecordBatchBuilder b;
        b.declare_schema({{"x", ColumnType::String}});
        b.append_int64(0, 12);
        b.end_row();
        b.append_string(0, "z");
        b.end_row();
        auto result = b.finish();
        REQUIRE(result.valid());
        Cells c(result);
        CHECK(c.str(0, 0) == "12");
        CHECK(c.str(0, 1) == "z");
    }
    SUBCASE("a declared Bool column holds a string as a null") {
        RecordBatchBuilder b;
        b.declare_schema({{"x", ColumnType::Bool}});
        b.append_string(0, "no");
        b.end_row();
        b.append_bool(0, true);
        b.end_row();
        auto result = b.finish();
        REQUIRE(result.valid());
        Cells c(result);
        CHECK(c.is_null(0, 0));
    }
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW

TEST_CASE("RecordBatchBuilder - a number promoted to text is normalized") {
    RecordBatchBuilder b;
    auto x = b.add_or_get_column("x", ColumnType::Double);
    const double values[] = {1.5,
                             2.0,
                             -0.0,
                             1e22,
                             9007199254740993.0,
                             std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity(),
                             0.1};
    for (const double v : values) {
        b.append_double(x, v);
        b.end_row();
    }
    x = b.add_or_get_column("x", ColumnType::String);
    b.append_string(x, "s");
    b.end_row();
    auto result = b.finish();
    REQUIRE(result.valid());
    Cells c(result);
    CHECK(c.str(0, 0) == "1.5");
    CHECK(c.str(0, 1) == "2.0");
    CHECK(c.str(0, 2) == "-0.0");
    CHECK(c.str(0, 3) == "1e+22");
    CHECK(c.str(0, 4) == "9007199254740992.0");
    CHECK(c.str(0, 5) == "Infinity");
    CHECK(c.str(0, 6) == "-Infinity");
    CHECK(c.str(0, 7) == "0.1");
    CHECK(c.str(0, 8) == "s");
}

TEST_CASE(
    "RecordBatchBuilder - an integer above 2^53 becomes an inexact double") {
    RecordBatchBuilder b;
    auto x = b.add_or_get_column("x", ColumnType::Int64);
    b.append_int64(x, 9007199254740993LL);
    b.end_row();
    x = b.add_or_get_column("x", ColumnType::Double);
    b.append_double(x, 0.5);
    b.end_row();
    auto result = b.finish();
    REQUIRE(result.valid());
    Cells c(result);
    CHECK(c.f64(0, 0) == 9007199254740992.0);
    CHECK(c.f64(0, 1) == 0.5);
}
