#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/utilities/common/arrow/array_view.h>
#include <dftracer/utils/utilities/common/arrow/arrow.h>
#include <doctest/doctest.h>
#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

using namespace dftracer::utils::utilities::common::arrow;

namespace {

struct Reader {
    ArrowArrayView av;
    explicit Reader(ArrowExportResult& r) {
        REQUIRE(init_array_view(av, r.get_schema(), r.get_array()) ==
                NANOARROW_OK);
    }
    ~Reader() { ArrowArrayViewReset(&av); }
    std::int64_t rows() const { return av.length; }
    std::int64_t i64(int c, std::int64_t r) const {
        return ArrowArrayViewGetIntUnsafe(av.children[c], r);
    }
    double dbl(int c, std::int64_t r) const {
        return ArrowArrayViewGetDoubleUnsafe(av.children[c], r);
    }
    bool null(int c, std::int64_t r) const {
        return ArrowArrayViewIsNull(av.children[c], r) != 0;
    }
    std::vector<std::string> str_list(int c, std::int64_t r) const {
        const ArrowArrayView* lv = av.children[c];
        const ArrowArrayView* elem = lv->children[0];
        std::int64_t s = ArrowArrayViewListChildOffset(lv, r);
        std::int64_t e = ArrowArrayViewListChildOffset(lv, r + 1);
        std::vector<std::string> out;
        for (std::int64_t p = s; p < e; ++p) {
            ArrowStringView v = ArrowArrayViewGetStringUnsafe(elem, p);
            out.emplace_back(v.data, static_cast<std::size_t>(v.size_bytes));
        }
        return out;
    }
};

// {p:INT64, o:INT64, v:INT64}; a nullopt v becomes a null cell.
ArrowExportResult make3(const std::vector<std::int64_t>& p,
                        const std::vector<std::int64_t>& o,
                        const std::vector<std::optional<std::int64_t>>& v) {
    RecordBatchBuilder b;
    b.declare_schema({{"p", ColumnType::INT64},
                      {"o", ColumnType::INT64},
                      {"v", ColumnType::INT64}});
    for (std::size_t i = 0; i < p.size(); ++i) {
        b.append_int64(0, p[i]);
        b.append_int64(1, o[i]);
        if (v[i])
            b.append_int64(2, *v[i]);
        else
            b.append_null(2);
        b.end_row();
    }
    return b.finish();
}

// {p:INT64, o:INT64, v:UINT64}
ArrowExportResult make3u(const std::vector<std::int64_t>& p,
                         const std::vector<std::int64_t>& o,
                         const std::vector<std::uint64_t>& v) {
    RecordBatchBuilder b;
    b.declare_schema({{"p", ColumnType::INT64},
                      {"o", ColumnType::INT64},
                      {"v", ColumnType::UINT64}});
    for (std::size_t i = 0; i < p.size(); ++i) {
        b.append_int64(0, p[i]);
        b.append_int64(1, o[i]);
        b.append_uint64(2, v[i]);
        b.end_row();
    }
    return b.finish();
}

ArrowExportResult win(ArrowExportResult& in, std::vector<std::uint32_t> part,
                      std::vector<std::uint32_t> order,
                      std::vector<WindowSpec> specs) {
    return window(in.get_schema(), in.get_array(), part.data(),
                  static_cast<std::uint32_t>(part.size()), order.data(),
                  static_cast<std::uint32_t>(order.size()), specs.data(),
                  static_cast<std::uint32_t>(specs.size()));
}

}  // namespace

TEST_CASE("window - row_number resets per partition") {
    auto in =
        make3({1, 1, 1, 2, 2}, {10, 20, 30, 10, 20}, {{}, {}, {}, {}, {}});
    auto out =
        win(in, {0}, {1}, {window_spec(WindowFunc::ROW_NUMBER, 0, "rn")});
    REQUIRE(out.num_columns() == 4);
    CHECK(std::string(out.get_schema()->children[3]->name) == "rn");
    Reader rd(out);
    REQUIRE(rd.rows() == 5);
    // Sorted (p,o): p1[10,20,30] -> 1,2,3 ; p2[10,20] -> 1,2
    std::vector<std::int64_t> want = {1, 2, 3, 1, 2};
    for (std::int64_t i = 0; i < 5; ++i) CHECK(rd.i64(3, i) == want[i]);
}

TEST_CASE("window - rank vs dense_rank on ties") {
    auto in = make3({1, 1, 1}, {10, 10, 20}, {{}, {}, {}});
    auto out = win(in, {0}, {1},
                   {window_spec(WindowFunc::RANK, 0, "rk"),
                    window_spec(WindowFunc::DENSE_RANK, 0, "dr")});
    Reader rd(out);
    REQUIRE(rd.rows() == 3);
    std::vector<std::int64_t> rk = {1, 1, 3};
    std::vector<std::int64_t> dr = {1, 1, 2};
    for (std::int64_t i = 0; i < 3; ++i) {
        CHECK(rd.i64(3, i) == rk[i]);
        CHECK(rd.i64(4, i) == dr[i]);
    }
}

TEST_CASE("window - lag/lead boundary null and interior shift") {
    auto in = make3({1, 1, 1}, {1, 2, 3}, {10, 20, 30});
    auto out = win(in, {0}, {1},
                   {window_spec(WindowFunc::LAG, 2, "lag", 1),
                    window_spec(WindowFunc::LEAD, 2, "lead", 1)});
    Reader rd(out);
    REQUIRE(rd.rows() == 3);
    // lag(v,1): [null,10,20]
    CHECK(rd.null(3, 0));
    CHECK(rd.i64(3, 1) == 10);
    CHECK(rd.i64(3, 2) == 20);
    // lead(v,1): [20,30,null]
    CHECK(rd.i64(4, 0) == 20);
    CHECK(rd.i64(4, 1) == 30);
    CHECK(rd.null(4, 2));
}

TEST_CASE("window - running sum resets per partition") {
    auto in = make3({1, 1, 2}, {1, 2, 1}, {10, 20, 100});
    auto out =
        win(in, {0}, {1}, {window_spec(WindowFunc::RUNNING_SUM, 2, "rs")});
    Reader rd(out);
    REQUIRE(rd.rows() == 3);
    CHECK(rd.i64(3, 0) == 10);   // p1
    CHECK(rd.i64(3, 1) == 30);   // p1 cumulative
    CHECK(rd.i64(3, 2) == 100);  // p2 reset
}

TEST_CASE("window - running min/max/count") {
    auto in = make3({1, 1, 1}, {1, 2, 3}, {30, 10, 20});
    auto out = win(in, {0}, {1},
                   {window_spec(WindowFunc::RUNNING_MIN, 2, "mn"),
                    window_spec(WindowFunc::RUNNING_MAX, 2, "mx"),
                    window_spec(WindowFunc::RUNNING_COUNT, 0, "cnt")});
    Reader rd(out);
    REQUIRE(rd.rows() == 3);
    std::vector<std::int64_t> mn = {30, 10, 10};
    std::vector<std::int64_t> mx = {30, 30, 30};
    std::vector<std::int64_t> cnt = {1, 2, 3};
    for (std::int64_t i = 0; i < 3; ++i) {
        CHECK(rd.i64(3, i) == mn[i]);
        CHECK(rd.i64(4, i) == mx[i]);
        CHECK(rd.i64(5, i) == cnt[i]);
    }
}

TEST_CASE("window - running sum skips null cell, count is count(*)") {
    auto in = make3({1, 1, 1}, {1, 2, 3}, {10, {}, 20});
    auto out = win(in, {0}, {1},
                   {window_spec(WindowFunc::RUNNING_SUM, 2, "rs"),
                    window_spec(WindowFunc::RUNNING_COUNT, 0, "cnt")});
    Reader rd(out);
    REQUIRE(rd.rows() == 3);
    std::vector<std::int64_t> rs = {10, 10, 30};  // null does not contribute
    std::vector<std::int64_t> cnt = {1, 2, 3};    // counts the null row too
    for (std::int64_t i = 0; i < 3; ++i) {
        CHECK(rd.i64(3, i) == rs[i]);
        CHECK(rd.i64(4, i) == cnt[i]);
    }
}

TEST_CASE("window - partition values do not leak across boundaries") {
    // p1 has a large value; p2's running max must not see it.
    auto in = make3({1, 2, 2}, {1, 1, 2}, {999, 5, 7});
    auto out = win(in, {0}, {1},
                   {window_spec(WindowFunc::RUNNING_MAX, 2, "mx"),
                    window_spec(WindowFunc::ROW_NUMBER, 0, "rn")});
    Reader rd(out);
    REQUIRE(rd.rows() == 3);
    // sorted: p1[1]=999 ; p2[1]=5, p2[2]=7
    CHECK((rd.i64(3, 0) == 999 && rd.i64(4, 0) == 1));
    CHECK((rd.i64(3, 1) == 5 && rd.i64(4, 1) == 1));
    CHECK((rd.i64(3, 2) == 7 && rd.i64(4, 2) == 2));
}

TEST_CASE("window - determinism across input order") {
    auto a = make3({1, 1, 2}, {10, 20, 5}, {1, 2, 3});
    auto b = make3({2, 1, 1}, {5, 20, 10}, {3, 2, 1});
    auto oa = win(a, {0}, {1}, {window_spec(WindowFunc::ROW_NUMBER, 0, "rn")});
    auto ob = win(b, {0}, {1}, {window_spec(WindowFunc::ROW_NUMBER, 0, "rn")});
    Reader ra(oa), rb(ob);
    REQUIRE(ra.rows() == 3);
    REQUIRE(rb.rows() == 3);
    for (std::int64_t i = 0; i < 3; ++i) {
        CHECK(ra.i64(0, i) == rb.i64(0, i));  // p
        CHECK(ra.i64(1, i) == rb.i64(1, i));  // o
        CHECK(ra.i64(2, i) == rb.i64(2, i));  // v
        CHECK(ra.i64(3, i) == rb.i64(3, i));  // rn
    }
}

TEST_CASE("window - multiple specs appended in order") {
    auto in = make3({1, 1, 1}, {1, 2, 3}, {10, 20, 30});
    auto out = win(in, {0}, {1},
                   {window_spec(WindowFunc::ROW_NUMBER, 0, "rn"),
                    window_spec(WindowFunc::RUNNING_SUM, 2, "rs"),
                    window_spec(WindowFunc::LAG, 2, "lag", 1)});
    REQUIRE(out.num_columns() == 6);
    CHECK(std::string(out.get_schema()->children[3]->name) == "rn");
    CHECK(std::string(out.get_schema()->children[4]->name) == "rs");
    CHECK(std::string(out.get_schema()->children[5]->name) == "lag");
    Reader rd(out);
    REQUIRE(rd.rows() == 3);
    CHECK((rd.i64(3, 0) == 1 && rd.i64(4, 0) == 10 && rd.null(5, 0)));
    CHECK((rd.i64(3, 1) == 2 && rd.i64(4, 1) == 30 && rd.i64(5, 1) == 10));
    CHECK((rd.i64(3, 2) == 3 && rd.i64(4, 2) == 60 && rd.i64(5, 2) == 20));
}

TEST_CASE("window - list<utf8> carry-through survives") {
    RecordBatchBuilder b;
    b.declare_schema({{"p", ColumnType::INT64},
                      {"o", ColumnType::INT64},
                      {"tags", ColumnType::STRING_LIST}});
    b.append_int64(0, 1);
    b.append_int64(1, 20);
    b.append_string_list(2, {std::string_view("r")});
    b.end_row();
    b.append_int64(0, 1);
    b.append_int64(1, 10);
    b.append_string_list(2, {std::string_view("p"), std::string_view("q")});
    b.end_row();
    auto in = b.finish();
    auto out =
        win(in, {0}, {1}, {window_spec(WindowFunc::ROW_NUMBER, 0, "rn")});
    Reader rd(out);
    REQUIRE(rd.rows() == 2);
    // sorted by o: row o=10 first (tags p,q), then o=20 (tags r)
    CHECK(rd.str_list(2, 0) == std::vector<std::string>{"p", "q"});
    CHECK(rd.i64(3, 0) == 1);
    CHECK(rd.str_list(2, 1) == std::vector<std::string>{"r"});
    CHECK(rd.i64(3, 1) == 2);
}

TEST_CASE("window - running sum over double column yields double") {
    RecordBatchBuilder b;
    b.declare_schema({{"o", ColumnType::INT64}, {"v", ColumnType::DOUBLE}});
    b.append_int64(0, 1);
    b.append_double(1, 1.5);
    b.end_row();
    b.append_int64(0, 2);
    b.append_double(1, 2.25);
    b.end_row();
    auto in = b.finish();
    auto out =
        win(in, {}, {0}, {window_spec(WindowFunc::RUNNING_SUM, 1, "rs")});
    Reader rd(out);
    REQUIRE(rd.rows() == 2);
    CHECK(rd.dbl(2, 0) == doctest::Approx(1.5));
    CHECK(rd.dbl(2, 1) == doctest::Approx(3.75));
}

TEST_CASE("window - delta discrete difference, first row null") {
    auto in = make3({1, 1, 1}, {1, 2, 3}, {10, 15, 13});
    auto out = win(in, {0}, {1}, {window_spec(WindowFunc::DELTA, 2, "d")});
    REQUIRE(out.num_columns() == 4);
    CHECK(std::string(out.get_schema()->children[3]->name) == "d");
    Reader rd(out);
    REQUIRE(rd.rows() == 3);
    CHECK(rd.null(3, 0));
    CHECK(rd.i64(3, 1) == 5);
    CHECK(rd.i64(3, 2) == -2);
}

TEST_CASE("window - delta on unsigned column yields signed negative") {
    // Counter drops 100 -> 30: delta must be -70, not a wrapped huge unsigned
    // value. Output of an integer-column delta is INT64 (Arrow format "l").
    auto in = make3u({1, 1, 1}, {1, 2, 3}, {100, 30, 45});
    auto out = win(in, {0}, {1}, {window_spec(WindowFunc::DELTA, 2, "d")});
    REQUIRE(out.num_columns() == 4);
    CHECK(std::string(out.get_schema()->children[3]->format) == "l");
    Reader rd(out);
    REQUIRE(rd.rows() == 3);
    CHECK(rd.null(3, 0));
    CHECK(rd.i64(3, 1) == -70);
    CHECK(rd.i64(3, 2) == 15);
}

TEST_CASE("window - rate on a decreasing unsigned column is correct") {
    auto in = make3u({1, 1}, {0, 10}, {100, 30});
    auto out = win(in, {0}, {1}, {rate_spec(2, 1, "rate", false)});
    Reader rd(out);
    REQUIRE(rd.rows() == 2);
    CHECK(rd.null(3, 0));
    CHECK(rd.dbl(3, 1) == doctest::Approx(-7.0));  // (30-100)/10, no wrap
}

TEST_CASE("window - delta resets per partition and nulls on null endpoint") {
    auto in = make3({1, 1, 1, 2, 2}, {1, 2, 3, 1, 2}, {10, {}, 20, 5, 7});
    auto out = win(in, {0}, {1}, {window_spec(WindowFunc::DELTA, 2, "d")});
    Reader rd(out);
    REQUIRE(rd.rows() == 5);
    CHECK(rd.null(3, 0));  // first row of p1
    CHECK(rd.null(3, 1));  // current cell null
    CHECK(rd.null(3, 2));  // previous cell null
    CHECK(rd.null(3, 3));  // first row of p2
    CHECK(rd.i64(3, 4) == 2);
}

TEST_CASE("window - rate over time, zero dt yields null") {
    auto in = make3({1, 1, 1}, {0, 10, 10}, {100, 150, 150});
    auto out = win(in, {0}, {1}, {rate_spec(2, 1, "rate", false)});
    REQUIRE(out.num_columns() == 4);
    Reader rd(out);
    REQUIRE(rd.rows() == 3);
    CHECK(rd.null(3, 0));                         // first row
    CHECK(rd.dbl(3, 1) == doctest::Approx(5.0));  // (150-100)/(10-0)
    CHECK(rd.null(3, 2));                         // dt == 0
}

TEST_CASE("window - rate counter reset uses raw value, resets per partition") {
    auto in = make3({1, 1, 2, 2}, {0, 10, 0, 10}, {100, 30, 5, 5});
    auto out = win(in, {0}, {1}, {rate_spec(2, 1, "rate", true)});
    Reader rd(out);
    REQUIRE(rd.rows() == 4);
    CHECK(rd.null(3, 0));
    CHECK(rd.dbl(3, 1) == doctest::Approx(3.0));  // drop -> dv=30, /10
    CHECK(rd.null(3, 2));                         // first row of p2, no leak
    CHECK(rd.dbl(3, 3) == doctest::Approx(0.0));
}

TEST_CASE("window - sessionize splits on gap strictly greater than threshold") {
    auto in = make3({1, 1, 1, 1}, {0, 5, 100, 105}, {0, 0, 0, 0});
    auto out = win(in, {0}, {1}, {session_spec(1, "sid", 50.0)});
    REQUIRE(out.num_columns() == 4);
    Reader rd(out);
    REQUIRE(rd.rows() == 4);
    std::vector<std::int64_t> want = {1, 1, 2, 2};
    for (std::int64_t i = 0; i < 4; ++i) CHECK(rd.i64(3, i) == want[i]);
}

TEST_CASE("window - sessionize gap equal to threshold does not split") {
    auto in = make3({1, 1, 2, 2}, {0, 50, 0, 200}, {0, 0, 0, 0});
    auto out = win(in, {0}, {1}, {session_spec(1, "sid", 50.0)});
    Reader rd(out);
    REQUIRE(rd.rows() == 4);
    std::vector<std::int64_t> want = {1, 1, 1, 2};  // p2 restarts at 1
    for (std::int64_t i = 0; i < 4; ++i) CHECK(rd.i64(3, i) == want[i]);
}

TEST_CASE("window - sessionize measures the gap from the latest end") {
    // With the end column the second row starts 30 after the end 120; by
    // start times alone it would be 150 after the first.
    auto in = make3({1, 1}, {0, 150}, {120, 155});
    auto ends = win(in, {0}, {1}, {session_spec(1, "sid", 50.0, 0.0, 2)});
    Reader re(ends);
    CHECK(re.i64(3, 0) == 1);
    CHECK(re.i64(3, 1) == 1);
    auto starts = win(in, {0}, {1}, {session_spec(1, "sid", 50.0)});
    Reader rs(starts);
    CHECK(rs.i64(3, 1) == 2);
}

TEST_CASE("window - sessionize gap between starts without an end column") {
    auto in = make3({1, 1, 1}, {0, 10, 25}, {0, 0, 0});
    auto out = win(in, {0}, {1}, {session_spec(1, "sid", 15.0)});
    Reader rd(out);
    for (std::int64_t i = 0; i < 3; ++i) CHECK(rd.i64(3, i) == 1);
}

TEST_CASE("window - sessionize splits a session longer than the span") {
    auto in = make3({1, 1, 1, 1}, {0, 50, 100, 150}, {0, 0, 0, 0});
    auto out = win(in, {0}, {1}, {session_spec(1, "sid", 60.0, 120.0)});
    Reader rd(out);
    std::vector<std::int64_t> want = {1, 1, 1, 2};
    for (std::int64_t i = 0; i < 4; ++i) CHECK(rd.i64(3, i) == want[i]);
}

TEST_CASE("window - sessionize gives a null time a null session") {
    // Time is column 2, ordered by column 1.
    auto in = make3({1, 1, 1}, {0, 1, 2}, {0, std::nullopt, 300});
    auto out = win(in, {0}, {1}, {session_spec(2, "sid", 100.0)});
    Reader rd(out);
    CHECK(rd.i64(3, 0) == 1);
    CHECK(rd.null(3, 1));
    CHECK(rd.i64(3, 2) == 2);
}

TEST_CASE("window - sessionize rejects a negative gap and a text end") {
    auto in = make3({1, 1}, {0, 10}, {0, 0});
    CHECK_THROWS_AS(win(in, {0}, {1}, {session_spec(1, "sid", -1.0)}),
                    dftracer::utils::DFTUtilsException);
    CHECK_THROWS_AS(win(in, {0}, {1}, {session_spec(1, "sid", 1.0, -2.0)}),
                    dftracer::utils::DFTUtilsException);
    RecordBatchBuilder b;
    b.declare_schema({{"p", ColumnType::INT64},
                      {"o", ColumnType::INT64},
                      {"e", ColumnType::STRING}});
    b.append_int64(0, 1);
    b.append_int64(1, 0);
    b.append_string(2, "x");
    b.end_row();
    auto text = b.finish();
    CHECK_THROWS_AS(win(text, {0}, {1}, {session_spec(1, "sid", 1.0, 0.0, 2)}),
                    dftracer::utils::DFTUtilsException);
}

TEST_CASE("window - delta determinism across input order") {
    auto a = make3({1, 1, 2}, {1, 2, 1}, {10, 15, 100});
    auto b = make3({2, 1, 1}, {1, 1, 2}, {100, 10, 15});
    auto oa = win(a, {0}, {1}, {window_spec(WindowFunc::DELTA, 2, "d")});
    auto ob = win(b, {0}, {1}, {window_spec(WindowFunc::DELTA, 2, "d")});
    Reader ra(oa), rb(ob);
    REQUIRE(ra.rows() == 3);
    REQUIRE(rb.rows() == 3);
    for (std::int64_t i = 0; i < 3; ++i) {
        CHECK(ra.null(3, i) == rb.null(3, i));
        if (!ra.null(3, i)) CHECK(ra.i64(3, i) == rb.i64(3, i));
    }
}

TEST_CASE("window - delta carries a list<utf8> column through") {
    RecordBatchBuilder b;
    b.declare_schema({{"p", ColumnType::INT64},
                      {"o", ColumnType::INT64},
                      {"v", ColumnType::INT64},
                      {"tags", ColumnType::STRING_LIST}});
    b.append_int64(0, 1);
    b.append_int64(1, 20);
    b.append_int64(2, 15);
    b.append_string_list(3, {std::string_view("r")});
    b.end_row();
    b.append_int64(0, 1);
    b.append_int64(1, 10);
    b.append_int64(2, 10);
    b.append_string_list(3, {std::string_view("p"), std::string_view("q")});
    b.end_row();
    auto in = b.finish();
    auto out = win(in, {0}, {1}, {window_spec(WindowFunc::DELTA, 2, "d")});
    Reader rd(out);
    REQUIRE(rd.rows() == 2);
    // sorted by o: o=10 first (tags p,q, delta null), then o=20 (delta 5)
    CHECK(rd.str_list(3, 0) == std::vector<std::string>{"p", "q"});
    CHECK(rd.null(4, 0));
    CHECK(rd.str_list(3, 1) == std::vector<std::string>{"r"});
    CHECK(rd.i64(4, 1) == 5);
}

TEST_CASE("window - frame_sum rows between 1 preceding and 1 following") {
    auto in = make3({1, 1, 1, 1}, {1, 2, 3, 4}, {1, 2, 3, 4});
    auto out = win(in, {0}, {1},
                   {frame_spec(WindowFunc::FRAME_SUM, 2, "fs", 1, 1, 0)});
    REQUIRE(out.num_columns() == 4);
    Reader rd(out);
    REQUIRE(rd.rows() == 4);
    std::vector<std::int64_t> want = {3, 6, 9, 7};  // clamped at edges
    for (std::int64_t i = 0; i < 4; ++i) CHECK(rd.i64(3, i) == want[i]);
}

TEST_CASE("window - frame_min/max/mean over 1 preceding and 1 following") {
    auto in = make3({1, 1, 1, 1}, {1, 2, 3, 4}, {1, 2, 3, 4});
    auto out = win(in, {0}, {1},
                   {frame_spec(WindowFunc::FRAME_MIN, 2, "mn", 1, 1, 0),
                    frame_spec(WindowFunc::FRAME_MAX, 2, "mx", 1, 1, 0),
                    frame_spec(WindowFunc::FRAME_MEAN, 2, "me", 1, 1, 0)});
    Reader rd(out);
    REQUIRE(rd.rows() == 4);
    std::vector<std::int64_t> mn = {1, 1, 2, 3};
    std::vector<std::int64_t> mx = {2, 3, 4, 4};
    std::vector<double> me = {1.5, 2.0, 3.0, 3.5};
    for (std::int64_t i = 0; i < 4; ++i) {
        CHECK(rd.i64(3, i) == mn[i]);
        CHECK(rd.i64(4, i) == mx[i]);
        CHECK(rd.dbl(5, i) == doctest::Approx(me[i]));
    }
}

TEST_CASE("window - frame_count skips null and frame_sum skips null in frame") {
    auto in = make3({1, 1, 1, 1}, {1, 2, 3, 4}, {1, {}, 3, 4});
    auto out = win(in, {0}, {1},
                   {frame_spec(WindowFunc::FRAME_COUNT, 2, "fc", 1, 1, 0),
                    frame_spec(WindowFunc::FRAME_SUM, 2, "fs", 1, 1, 0)});
    Reader rd(out);
    REQUIRE(rd.rows() == 4);
    std::vector<std::int64_t> fc = {1, 2, 2, 2};
    std::vector<std::int64_t> fs = {1, 4, 7, 7};
    for (std::int64_t i = 0; i < 4; ++i) {
        CHECK(rd.i64(3, i) == fc[i]);
        CHECK(rd.i64(4, i) == fs[i]);
    }
}

TEST_CASE("window - frame_sum all-null frame yields null") {
    auto in = make3({1, 1, 1}, {1, 2, 3}, {{}, {}, {}});
    auto out = win(in, {0}, {1},
                   {frame_spec(WindowFunc::FRAME_SUM, 2, "fs", 1, 1, 0)});
    Reader rd(out);
    REQUIRE(rd.rows() == 3);
    for (std::int64_t i = 0; i < 3; ++i) CHECK(rd.null(3, i));
}

TEST_CASE("window - frame_sum resets per partition") {
    auto in = make3({1, 1, 2, 2}, {1, 2, 1, 2}, {1, 2, 10, 20});
    auto out = win(in, {0}, {1},
                   {frame_spec(WindowFunc::FRAME_SUM, 2, "fs", 1, 1, 0)});
    Reader rd(out);
    REQUIRE(rd.rows() == 4);
    // p1: {1,2}->3, {1,2}->3 ; p2: {10,20}->30, {10,20}->30
    std::vector<std::int64_t> want = {3, 3, 30, 30};
    for (std::int64_t i = 0; i < 4; ++i) CHECK(rd.i64(3, i) == want[i]);
}

TEST_CASE("window - unbounded preceding frame_sum equals running_sum") {
    auto in = make3({1, 1, 1, 2}, {1, 2, 3, 1}, {10, 20, 30, 100});
    auto out = win(
        in, {0}, {1},
        {window_spec(WindowFunc::RUNNING_SUM, 2, "rs"),
         frame_spec(WindowFunc::FRAME_SUM, 2, "fs", WINDOW_UNBOUNDED, 0, 0)});
    Reader rd(out);
    REQUIRE(rd.rows() == 4);
    for (std::int64_t i = 0; i < 4; ++i) CHECK(rd.i64(3, i) == rd.i64(4, i));
}

TEST_CASE("window - ntile splits partition larger buckets first") {
    auto a = make3({1, 1, 1, 1}, {1, 2, 3, 4}, {0, 0, 0, 0});
    auto oa = win(a, {0}, {1}, {window_spec(WindowFunc::NTILE, 0, "nt", 2)});
    Reader ra(oa);
    REQUIRE(ra.rows() == 4);
    std::vector<std::int64_t> w2 = {1, 1, 2, 2};
    for (std::int64_t i = 0; i < 4; ++i) CHECK(ra.i64(3, i) == w2[i]);

    auto b = make3({1, 1, 1, 1, 1}, {1, 2, 3, 4, 5}, {0, 0, 0, 0, 0});
    auto ob = win(b, {0}, {1}, {window_spec(WindowFunc::NTILE, 0, "nt", 3)});
    Reader rb(ob);
    REQUIRE(rb.rows() == 5);
    std::vector<std::int64_t> w3 = {1, 1, 2, 2, 3};  // sizes 2,2,1
    for (std::int64_t i = 0; i < 5; ++i) CHECK(rb.i64(3, i) == w3[i]);
}

TEST_CASE("window - first/last/nth value over partition") {
    auto in = make3({1, 1, 1, 2, 2}, {1, 2, 3, 1, 2}, {10, 20, 30, 40, 50});
    auto out = win(in, {0}, {1},
                   {window_spec(WindowFunc::FIRST_VALUE, 2, "fv"),
                    window_spec(WindowFunc::LAST_VALUE, 2, "lv"),
                    window_spec(WindowFunc::NTH_VALUE, 2, "nth", 2)});
    Reader rd(out);
    REQUIRE(rd.rows() == 5);
    // p1 rows 0..2: first=10 last=30 nth2=20 ; p2 rows 3..4: first=40 last=50
    std::vector<std::int64_t> fv = {10, 10, 10, 40, 40};
    std::vector<std::int64_t> lv = {30, 30, 30, 50, 50};
    for (std::int64_t i = 0; i < 5; ++i) {
        CHECK(rd.i64(3, i) == fv[i]);
        CHECK(rd.i64(4, i) == lv[i]);
    }
    // nth(2): p1 -> 20 for all three rows; p2 has 2 rows -> 50
    CHECK(rd.i64(5, 0) == 20);
    CHECK(rd.i64(5, 3) == 50);
}

TEST_CASE("window - nth_value out of range yields null") {
    auto in = make3({1, 1}, {1, 2}, {10, 20});
    auto out =
        win(in, {0}, {1}, {window_spec(WindowFunc::NTH_VALUE, 2, "nth", 3)});
    Reader rd(out);
    REQUIRE(rd.rows() == 2);
    CHECK(rd.null(2 + 1, 0));  // column 3
    CHECK(rd.null(3, 1));
}

TEST_CASE("window - first/last/nth carry a list<utf8> column") {
    RecordBatchBuilder b;
    b.declare_schema({{"p", ColumnType::INT64},
                      {"o", ColumnType::INT64},
                      {"tags", ColumnType::STRING_LIST}});
    b.append_int64(0, 1);
    b.append_int64(1, 20);
    b.append_string_list(2, {std::string_view("r")});
    b.end_row();
    b.append_int64(0, 1);
    b.append_int64(1, 10);
    b.append_string_list(2, {std::string_view("p"), std::string_view("q")});
    b.end_row();
    auto in = b.finish();
    auto out = win(in, {0}, {1},
                   {window_spec(WindowFunc::FIRST_VALUE, 2, "fv"),
                    window_spec(WindowFunc::LAST_VALUE, 2, "lv"),
                    window_spec(WindowFunc::NTH_VALUE, 2, "nth", 2)});
    Reader rd(out);
    REQUIRE(rd.rows() == 2);
    // sorted by o: first row o=10 (p,q), last o=20 (r)
    CHECK(rd.str_list(3, 0) == std::vector<std::string>{"p", "q"});  // first
    CHECK(rd.str_list(4, 0) == std::vector<std::string>{"r"});       // last
    CHECK(rd.str_list(5, 0) == std::vector<std::string>{"r"});       // nth 2
}

TEST_CASE("window - frame determinism across input order") {
    auto a = make3({1, 1, 1, 2}, {1, 2, 3, 1}, {1, 2, 3, 9});
    auto b = make3({2, 1, 1, 1}, {1, 3, 1, 2}, {9, 3, 1, 2});
    auto oa =
        win(a, {0}, {1}, {frame_spec(WindowFunc::FRAME_SUM, 2, "fs", 1, 1, 0)});
    auto ob =
        win(b, {0}, {1}, {frame_spec(WindowFunc::FRAME_SUM, 2, "fs", 1, 1, 0)});
    Reader ra(oa), rb(ob);
    REQUIRE(ra.rows() == 4);
    REQUIRE(rb.rows() == 4);
    for (std::int64_t i = 0; i < 4; ++i) CHECK(ra.i64(3, i) == rb.i64(3, i));
}

TEST_CASE("window - wide frame 3 preceding 2 following exercises deque") {
    // Decreasing then increasing with duplicates stresses min/max eviction.
    std::vector<std::optional<std::int64_t>> v = {5, 5, 3, 1, 2, 2, 4, 6};
    auto in = make3({1, 1, 1, 1, 1, 1, 1, 1}, {1, 2, 3, 4, 5, 6, 7, 8}, v);
    auto out = win(in, {0}, {1},
                   {frame_spec(WindowFunc::FRAME_SUM, 2, "fs", 3, 2, 0),
                    frame_spec(WindowFunc::FRAME_MIN, 2, "mn", 3, 2, 0),
                    frame_spec(WindowFunc::FRAME_MAX, 2, "mx", 3, 2, 0),
                    frame_spec(WindowFunc::FRAME_COUNT, 2, "fc", 3, 2, 0),
                    frame_spec(WindowFunc::FRAME_MEAN, 2, "me", 3, 2, 0)});
    Reader rd(out);
    REQUIRE(rd.rows() == 8);
    std::vector<std::int64_t> fs = {13, 14, 16, 18, 17, 18, 15, 14};
    std::vector<std::int64_t> mn = {3, 1, 1, 1, 1, 1, 1, 2};
    std::vector<std::int64_t> mx = {5, 5, 5, 5, 5, 6, 6, 6};
    std::vector<std::int64_t> fc = {3, 4, 5, 6, 6, 6, 5, 4};
    for (std::int64_t i = 0; i < 8; ++i) {
        CHECK(rd.i64(3, i) == fs[i]);
        CHECK(rd.i64(4, i) == mn[i]);
        CHECK(rd.i64(5, i) == mx[i]);
        CHECK(rd.i64(6, i) == fc[i]);
        CHECK(rd.dbl(7, i) == doctest::Approx(static_cast<double>(fs[i]) /
                                              static_cast<double>(fc[i])));
    }
}

TEST_CASE("window - frame slides across a null cell") {
    auto in = make3({1, 1, 1, 1}, {1, 2, 3, 4}, {10, {}, 20, 30});
    auto out = win(in, {0}, {1},
                   {frame_spec(WindowFunc::FRAME_SUM, 2, "fs", 1, 1, 0),
                    frame_spec(WindowFunc::FRAME_MIN, 2, "mn", 1, 1, 0),
                    frame_spec(WindowFunc::FRAME_MAX, 2, "mx", 1, 1, 0),
                    frame_spec(WindowFunc::FRAME_COUNT, 2, "fc", 1, 1, 0),
                    frame_spec(WindowFunc::FRAME_MEAN, 2, "me", 1, 1, 0)});
    Reader rd(out);
    REQUIRE(rd.rows() == 4);
    std::vector<std::int64_t> fs = {10, 30, 50, 50};
    std::vector<std::int64_t> mn = {10, 10, 20, 20};
    std::vector<std::int64_t> mx = {10, 20, 30, 30};
    std::vector<std::int64_t> fc = {1, 2, 2, 2};
    std::vector<double> me = {10.0, 15.0, 25.0, 25.0};
    for (std::int64_t i = 0; i < 4; ++i) {
        CHECK(rd.i64(3, i) == fs[i]);
        CHECK(rd.i64(4, i) == mn[i]);
        CHECK(rd.i64(5, i) == mx[i]);
        CHECK(rd.i64(6, i) == fc[i]);
        CHECK(rd.dbl(7, i) == doctest::Approx(me[i]));
    }
}

TEST_CASE("window - frame min/max reset per partition (no deque leak)") {
    auto in = make3({1, 1, 2, 2}, {1, 2, 1, 2}, {9, 1, 5, 3});
    auto out = win(in, {0}, {1},
                   {frame_spec(WindowFunc::FRAME_MIN, 2, "mn", 1, 1, 0),
                    frame_spec(WindowFunc::FRAME_MAX, 2, "mx", 1, 1, 0)});
    Reader rd(out);
    REQUIRE(rd.rows() == 4);
    std::vector<std::int64_t> mn = {1, 1, 3, 3};
    std::vector<std::int64_t> mx = {9, 9, 5, 5};
    for (std::int64_t i = 0; i < 4; ++i) {
        CHECK(rd.i64(3, i) == mn[i]);
        CHECK(rd.i64(4, i) == mx[i]);
    }
}

TEST_CASE("window - frame all-null yields null for min/max/mean/count-zero") {
    auto in = make3({1, 1, 1}, {1, 2, 3}, {{}, {}, {}});
    auto out = win(in, {0}, {1},
                   {frame_spec(WindowFunc::FRAME_MIN, 2, "mn", 1, 1, 0),
                    frame_spec(WindowFunc::FRAME_MAX, 2, "mx", 1, 1, 0),
                    frame_spec(WindowFunc::FRAME_MEAN, 2, "me", 1, 1, 0),
                    frame_spec(WindowFunc::FRAME_COUNT, 2, "fc", 1, 1, 0)});
    Reader rd(out);
    REQUIRE(rd.rows() == 3);
    for (std::int64_t i = 0; i < 3; ++i) {
        CHECK(rd.null(3, i));
        CHECK(rd.null(4, i));
        CHECK(rd.null(5, i));
        CHECK(rd.i64(6, i) == 0);
    }
}

TEST_CASE("window - large-n wide frame stays correct") {
    const std::int64_t n = 10000;
    const std::int64_t prec = 50, foll = 50;
    std::vector<std::int64_t> p(static_cast<std::size_t>(n), 1), o;
    std::vector<std::optional<std::int64_t>> v;
    for (std::int64_t i = 0; i < n; ++i) {
        o.push_back(i);
        v.push_back(i);
    }
    auto in = make3(p, o, v);
    auto out = win(in, {0}, {1},
                   {frame_spec(WindowFunc::FRAME_SUM, 2, "fs", prec, foll, 0),
                    frame_spec(WindowFunc::FRAME_MIN, 2, "mn", prec, foll, 0),
                    frame_spec(WindowFunc::FRAME_MAX, 2, "mx", prec, foll, 0)});
    Reader rd(out);
    REQUIRE(rd.rows() == n);
    for (std::int64_t i = 0; i < n; ++i) {
        std::int64_t lo = std::max<std::int64_t>(0, i - prec);
        std::int64_t hi = std::min<std::int64_t>(n - 1, i + foll);
        std::int64_t sum = 0;
        for (std::int64_t j = lo; j <= hi; ++j) sum += j;
        CHECK(rd.i64(3, i) == sum);
        CHECK(rd.i64(4, i) == lo);
        CHECK(rd.i64(5, i) == hi);
    }
}

TEST_CASE("window - frame_min/max determinism across input order") {
    auto a = make3({1, 1, 1, 2}, {1, 2, 3, 1}, {3, 1, 2, 9});
    auto b = make3({2, 1, 1, 1}, {1, 3, 1, 2}, {9, 2, 3, 1});
    auto oa = win(a, {0}, {1},
                  {frame_spec(WindowFunc::FRAME_MIN, 2, "mn", 1, 1, 0),
                   frame_spec(WindowFunc::FRAME_MAX, 2, "mx", 1, 1, 0)});
    auto ob = win(b, {0}, {1},
                  {frame_spec(WindowFunc::FRAME_MIN, 2, "mn", 1, 1, 0),
                   frame_spec(WindowFunc::FRAME_MAX, 2, "mx", 1, 1, 0)});
    Reader ra(oa), rb(ob);
    REQUIRE(ra.rows() == 4);
    REQUIRE(rb.rows() == 4);
    for (std::int64_t i = 0; i < 4; ++i) {
        CHECK(ra.i64(3, i) == rb.i64(3, i));
        CHECK(ra.i64(4, i) == rb.i64(4, i));
    }
}

TEST_CASE("window - percent_rank over ties and single-row partition") {
    auto in = make3({1, 1, 1, 1}, {10, 20, 20, 40}, {0, 0, 0, 0});
    auto out =
        win(in, {0}, {1}, {window_spec(WindowFunc::PERCENT_RANK, 0, "pr")});
    REQUIRE(out.num_columns() == 4);
    Reader rd(out);
    REQUIRE(rd.rows() == 4);
    std::vector<double> want = {0.0, 1.0 / 3.0, 1.0 / 3.0, 1.0};
    for (std::int64_t i = 0; i < 4; ++i)
        CHECK(rd.dbl(3, i) == doctest::Approx(want[i]));

    auto in2 = make3({7}, {5}, {0});
    auto out2 =
        win(in2, {0}, {1}, {window_spec(WindowFunc::PERCENT_RANK, 0, "pr")});
    Reader rd2(out2);
    REQUIRE(rd2.rows() == 1);
    CHECK(rd2.dbl(3, 0) == doctest::Approx(0.0));
}

TEST_CASE("window - cume_dist shares among peers and resets per partition") {
    auto in =
        make3({1, 1, 1, 1, 2, 2}, {10, 20, 20, 40, 5, 5}, {0, 0, 0, 0, 0, 0});
    auto out = win(in, {0}, {1}, {window_spec(WindowFunc::CUME_DIST, 0, "cd")});
    Reader rd(out);
    REQUIRE(rd.rows() == 6);
    std::vector<double> want = {0.25, 0.75, 0.75, 1.0, 1.0, 1.0};
    for (std::int64_t i = 0; i < 6; ++i)
        CHECK(rd.dbl(3, i) == doctest::Approx(want[i]));
}

TEST_CASE("window - range frame sum by value delta on the order column") {
    auto in = make3({1, 1, 1, 1}, {10, 12, 20, 21}, {1, 2, 3, 4});
    auto out = win(in, {0}, {1},
                   {frame_spec(WindowFunc::FRAME_SUM, 2, "fs", 5, 5, 0,
                               WindowFrameMode::RANGE)});
    Reader rd(out);
    REQUIRE(rd.rows() == 4);
    // 10 -> {10,12}=3 ; 12 -> {10,12}=3 ; 20 -> {20,21}=7 ; 21 -> {20,21}=7
    std::vector<std::int64_t> want = {3, 3, 7, 7};
    for (std::int64_t i = 0; i < 4; ++i) CHECK(rd.i64(3, i) == want[i]);
}

TEST_CASE("window - range unbounded-preceding includes peers, rows does not") {
    auto in = make3({1, 1, 1}, {10, 10, 20}, {1, 2, 3});
    auto out =
        win(in, {0}, {1},
            {window_spec(WindowFunc::RUNNING_SUM, 2, "rows"),
             frame_spec(WindowFunc::FRAME_SUM, 2, "rng", WINDOW_UNBOUNDED, 0, 0,
                        WindowFrameMode::RANGE)});
    Reader rd(out);
    REQUIRE(rd.rows() == 3);
    std::vector<std::int64_t> rows = {1, 3, 6};
    std::vector<std::int64_t> rng = {3, 3, 6};  // peers collapsed at the tie
    for (std::int64_t i = 0; i < 3; ++i) {
        CHECK(rd.i64(3, i) == rows[i]);
        CHECK(rd.i64(4, i) == rng[i]);
    }
    CHECK(rd.i64(3, 0) != rd.i64(4, 0));  // peer-inclusion difference on a tie
}

TEST_CASE("window - range frame over a double order column resets per part") {
    RecordBatchBuilder b;
    b.declare_schema({{"p", ColumnType::INT64},
                      {"o", ColumnType::DOUBLE},
                      {"v", ColumnType::INT64}});
    std::vector<std::int64_t> p = {1, 1, 2, 2};
    std::vector<double> o = {10.0, 12.0, 100.0, 101.0};
    std::vector<std::int64_t> v = {1, 2, 10, 20};
    for (std::size_t i = 0; i < p.size(); ++i) {
        b.append_int64(0, p[i]);
        b.append_double(1, o[i]);
        b.append_int64(2, v[i]);
        b.end_row();
    }
    auto in = b.finish();
    auto out = win(in, {0}, {1},
                   {frame_spec(WindowFunc::FRAME_SUM, 2, "fs", 5, 5, 0,
                               WindowFrameMode::RANGE)});
    Reader rd(out);
    REQUIRE(rd.rows() == 4);
    std::vector<std::int64_t> want = {3, 3, 30, 30};  // p2 does not see p1
    for (std::int64_t i = 0; i < 4; ++i) CHECK(rd.i64(3, i) == want[i]);
}

TEST_CASE("window - percent_rank/cume_dist/range determinism across order") {
    auto a = make3({1, 1, 1, 2}, {10, 20, 30, 5}, {1, 2, 3, 4});
    auto b = make3({2, 1, 1, 1}, {5, 30, 10, 20}, {4, 3, 1, 2});
    auto specs =
        std::vector<WindowSpec>{window_spec(WindowFunc::PERCENT_RANK, 0, "pr"),
                                window_spec(WindowFunc::CUME_DIST, 0, "cd"),
                                frame_spec(WindowFunc::FRAME_SUM, 2, "rng", 10,
                                           10, 0, WindowFrameMode::RANGE)};
    auto oa = win(a, {0}, {1}, specs);
    auto ob = win(b, {0}, {1}, specs);
    Reader ra(oa), rb(ob);
    REQUIRE(ra.rows() == 4);
    REQUIRE(rb.rows() == 4);
    for (std::int64_t i = 0; i < 4; ++i) {
        CHECK(ra.dbl(3, i) == doctest::Approx(rb.dbl(3, i)));
        CHECK(ra.dbl(4, i) == doctest::Approx(rb.dbl(4, i)));
        CHECK(ra.i64(5, i) == rb.i64(5, i));
    }
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW
