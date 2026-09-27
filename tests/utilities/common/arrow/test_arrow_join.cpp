#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/common/arrow/array_view.h>
#include <dftracer/utils/utilities/common/arrow/arrow.h>
#include <doctest/doctest.h>
#include <nanoarrow/nanoarrow.h>

#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace dftracer::utils::utilities::common::arrow;

namespace {

// A record batch read back for assertions; owns its ArrowArrayView.
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
    std::string str(int c, std::int64_t r) const {
        ArrowStringView s = ArrowArrayViewGetStringUnsafe(av.children[c], r);
        return std::string(s.data, static_cast<std::size_t>(s.size_bytes));
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
    std::vector<std::int64_t> i64_list(int c, std::int64_t r) const {
        const ArrowArrayView* lv = av.children[c];
        const ArrowArrayView* elem = lv->children[0];
        std::int64_t s = ArrowArrayViewListChildOffset(lv, r);
        std::int64_t e = ArrowArrayViewListChildOffset(lv, r + 1);
        std::vector<std::int64_t> out;
        for (std::int64_t p = s; p < e; ++p)
            out.push_back(ArrowArrayViewGetIntUnsafe(elem, p));
        return out;
    }
    ArrowType list_elem_type(int c) const {
        return av.children[c]->children[0]->storage_type;
    }
};

std::string name_of(ArrowExportResult& r, int c) {
    return r.get_schema()->children[c]->name;
}

// Build {kname:INT64, vname:STRING}; a nullopt key becomes a null key cell.
ArrowExportResult make_ks(const std::vector<std::optional<std::int64_t>>& keys,
                          const std::vector<std::string>& vals,
                          const char* kname, const char* vname) {
    RecordBatchBuilder b;
    b.declare_schema({{kname, ColumnType::INT64}, {vname, ColumnType::STRING}});
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i])
            b.append_int64(0, *keys[i]);
        else
            b.append_null(0);
        b.append_string(1, vals[i]);
        b.end_row();
    }
    return b.finish();
}

ArrowExportResult join_ks(ArrowExportResult& l, ArrowExportResult& r,
                          JoinType t) {
    std::uint32_t lk = 0, rk = 0;
    return join(l.get_schema(), l.get_array(), &lk, r.get_schema(),
                r.get_array(), &rk, 1, t);
}

// {k:INT64, vals:list<int32>} built via raw nanoarrow (RecordBatchBuilder has
// no list-of-int32 path).
ArrowExportResult make_k_i32list(
    const std::vector<std::int64_t>& keys,
    const std::vector<std::vector<std::int32_t>>& lists) {
    nanoarrow::UniqueSchema schema;
    REQUIRE(ArrowSchemaInitFromType(schema.get(), NANOARROW_TYPE_STRUCT) ==
            NANOARROW_OK);
    REQUIRE(ArrowSchemaAllocateChildren(schema.get(), 2) == NANOARROW_OK);
    REQUIRE(ArrowSchemaInitFromType(schema->children[0],
                                    NANOARROW_TYPE_INT64) == NANOARROW_OK);
    REQUIRE(ArrowSchemaSetName(schema->children[0], "k") == NANOARROW_OK);
    REQUIRE(ArrowSchemaInitFromType(schema->children[1], NANOARROW_TYPE_LIST) ==
            NANOARROW_OK);
    REQUIRE(ArrowSchemaSetType(schema->children[1]->children[0],
                               NANOARROW_TYPE_INT32) == NANOARROW_OK);
    REQUIRE(ArrowSchemaSetName(schema->children[1], "vals") == NANOARROW_OK);

    nanoarrow::UniqueArray array;
    REQUIRE(ArrowArrayInitFromSchema(array.get(), schema.get(), nullptr) ==
            NANOARROW_OK);
    REQUIRE(ArrowArrayStartAppending(array.get()) == NANOARROW_OK);
    ArrowArray* item = array->children[1]->children[0];
    for (std::size_t i = 0; i < keys.size(); ++i) {
        REQUIRE(ArrowArrayAppendInt(array->children[0], keys[i]) ==
                NANOARROW_OK);
        for (std::int32_t v : lists[i])
            REQUIRE(ArrowArrayAppendInt(item, v) == NANOARROW_OK);
        REQUIRE(ArrowArrayFinishElement(array->children[1]) == NANOARROW_OK);
        REQUIRE(ArrowArrayFinishElement(array.get()) == NANOARROW_OK);
    }
    REQUIRE(ArrowArrayFinishBuildingDefault(array.get(), nullptr) ==
            NANOARROW_OK);
    return ArrowExportResult(std::move(schema), std::move(array));
}

}  // namespace

TEST_CASE("join - 1:1 all four types") {
    auto l = make_ks({1, 2, 3}, {"a", "b", "c"}, "k", "lv");
    auto r = make_ks({2, 3, 4}, {"x", "y", "z"}, "k", "rv");

    SUBCASE("schema") {
        auto out = join_ks(l, r, JoinType::INNER);
        REQUIRE(out.num_columns() == 3);
        CHECK(name_of(out, 0) == "k");
        CHECK(name_of(out, 1) == "lv");
        CHECK(name_of(out, 2) == "rv");
    }
    SUBCASE("inner") {
        auto out = join_ks(l, r, JoinType::INNER);
        Reader rd(out);
        REQUIRE(rd.rows() == 2);
        CHECK(rd.i64(0, 0) == 2);
        CHECK(rd.str(1, 0) == "b");
        CHECK(rd.str(2, 0) == "x");
        CHECK(rd.i64(0, 1) == 3);
        CHECK(rd.str(1, 1) == "c");
        CHECK(rd.str(2, 1) == "y");
    }
    SUBCASE("left") {
        auto out = join_ks(l, r, JoinType::LEFT);
        Reader rd(out);
        REQUIRE(rd.rows() == 3);
        CHECK(rd.i64(0, 0) == 1);
        CHECK(rd.str(1, 0) == "a");
        CHECK(rd.null(2, 0));  // right value null-padded
        CHECK(rd.i64(0, 2) == 3);
        CHECK(rd.str(2, 2) == "y");
    }
    SUBCASE("right") {
        auto out = join_ks(l, r, JoinType::RIGHT);
        Reader rd(out);
        REQUIRE(rd.rows() == 3);
        CHECK(rd.i64(0, 2) == 4);
        CHECK(rd.null(1, 2));  // left value null-padded
        CHECK(rd.str(2, 2) == "z");
    }
    SUBCASE("full") {
        auto out = join_ks(l, r, JoinType::FULL);
        Reader rd(out);
        REQUIRE(rd.rows() == 4);
        CHECK(rd.i64(0, 0) == 1);
        CHECK(rd.null(2, 0));
        CHECK(rd.i64(0, 3) == 4);
        CHECK(rd.null(1, 3));
    }
}

TEST_CASE("join - 1:N and N:1") {
    auto l = make_ks({2}, {"b"}, "k", "lv");
    auto r = make_ks({2, 2}, {"x", "x2"}, "k", "rv");
    SUBCASE("1:N inner emits N rows") {
        auto out = join_ks(l, r, JoinType::INNER);
        Reader rd(out);
        REQUIRE(rd.rows() == 2);
        CHECK(rd.str(2, 0) == "x");
        CHECK(rd.str(2, 1) == "x2");
    }
    SUBCASE("N:1 inner emits N rows") {
        auto out = join_ks(r, l, JoinType::INNER);
        Reader rd(out);
        REQUIRE(rd.rows() == 2);
        CHECK(rd.str(1, 0) == "x");
        CHECK(rd.str(1, 1) == "x2");
    }
}

TEST_CASE("join - N:M cross product") {
    auto l = make_ks({2, 2}, {"b", "b2"}, "k", "lv");
    auto r = make_ks({2, 2}, {"x", "x2"}, "k", "rv");
    auto out = join_ks(l, r, JoinType::INNER);
    Reader rd(out);
    REQUIRE(rd.rows() == 4);
    // Deterministic order: left group order, then right group order.
    CHECK((rd.str(1, 0) == "b" && rd.str(2, 0) == "x"));
    CHECK((rd.str(1, 1) == "b" && rd.str(2, 1) == "x2"));
    CHECK((rd.str(1, 2) == "b2" && rd.str(2, 2) == "x"));
    CHECK((rd.str(1, 3) == "b2" && rd.str(2, 3) == "x2"));
}

TEST_CASE("join - determinism across input order") {
    auto l1 = make_ks({1, 2, 3}, {"a", "b", "c"}, "k", "lv");
    auto r1 = make_ks({1, 2, 3}, {"x", "y", "z"}, "k", "rv");
    auto l2 = make_ks({3, 1, 2}, {"c", "a", "b"}, "k", "lv");
    auto r2 = make_ks({2, 3, 1}, {"y", "z", "x"}, "k", "rv");
    auto o1 = join_ks(l1, r1, JoinType::INNER);
    auto o2 = join_ks(l2, r2, JoinType::INNER);
    Reader a(o1), b(o2);
    REQUIRE(a.rows() == 3);
    REQUIRE(b.rows() == 3);
    for (std::int64_t i = 0; i < 3; ++i) {
        CHECK(a.i64(0, i) == b.i64(0, i));
        CHECK(a.str(1, i) == b.str(1, i));
        CHECK(a.str(2, i) == b.str(2, i));
        CHECK(a.i64(0, i) == i + 1);  // ordered by key
    }
}

TEST_CASE("join - multi-column key") {
    RecordBatchBuilder lb;
    lb.declare_schema({{"i", ColumnType::INT64},
                       {"s", ColumnType::STRING},
                       {"lv", ColumnType::STRING}});
    auto addl = [&](std::int64_t i, const char* s, const char* v) {
        lb.append_int64(0, i);
        lb.append_string(1, s);
        lb.append_string(2, v);
        lb.end_row();
    };
    addl(1, "a", "l1");
    addl(1, "b", "l2");
    addl(2, "a", "l3");
    auto l = lb.finish();

    RecordBatchBuilder rb;
    rb.declare_schema({{"i", ColumnType::INT64},
                       {"s", ColumnType::STRING},
                       {"rv", ColumnType::STRING}});
    auto addr = [&](std::int64_t i, const char* s, const char* v) {
        rb.append_int64(0, i);
        rb.append_string(1, s);
        rb.append_string(2, v);
        rb.end_row();
    };
    addr(1, "b", "r1");
    addr(2, "a", "r2");
    addr(2, "c", "r3");
    auto r = rb.finish();

    std::uint32_t lk[2] = {0, 1};
    std::uint32_t rk[2] = {0, 1};
    auto out = join(l.get_schema(), l.get_array(), lk, r.get_schema(),
                    r.get_array(), rk, 2, JoinType::INNER);
    Reader rd(out);
    REQUIRE(rd.rows() == 2);
    // matched (1,"b") and (2,"a"); ordered by key
    CHECK((rd.i64(0, 0) == 1 && rd.str(1, 0) == "b"));
    CHECK((rd.str(2, 0) == "l2" && rd.str(3, 0) == "r1"));
    CHECK((rd.i64(0, 1) == 2 && rd.str(1, 1) == "a"));
    CHECK((rd.str(2, 1) == "l3" && rd.str(3, 1) == "r2"));
}

TEST_CASE("join - null key never matches") {
    auto l = make_ks({1, std::nullopt, 3}, {"a", "n", "c"}, "k", "lv");
    auto r = make_ks({1, 3}, {"x", "z"}, "k", "rv");
    SUBCASE("inner drops null key") {
        auto out = join_ks(l, r, JoinType::INNER);
        Reader rd(out);
        REQUIRE(rd.rows() == 2);
        for (std::int64_t i = 0; i < 2; ++i) CHECK(!rd.null(0, i));
    }
    SUBCASE("full keeps null key null-padded on right") {
        auto out = join_ks(l, r, JoinType::FULL);
        Reader rd(out);
        REQUIRE(rd.rows() == 3);
        bool found = false;
        for (std::int64_t i = 0; i < 3; ++i) {
            if (rd.null(0, i)) {
                found = true;
                CHECK(rd.str(1, i) == "n");
                CHECK(rd.null(2, i));  // right side padded
            }
        }
        CHECK(found);
    }
}

TEST_CASE("join - value name collision suffix") {
    auto l = make_ks({1, 2}, {"a", "b"}, "k", "name");
    auto r = make_ks({1, 2}, {"x", "y"}, "k", "name");
    auto out = join_ks(l, r, JoinType::INNER);
    REQUIRE(out.num_columns() == 3);
    CHECK(name_of(out, 0) == "k");
    CHECK(name_of(out, 1) == "name");
    CHECK(name_of(out, 2) == "name_right");
    Reader rd(out);
    CHECK(rd.str(1, 0) == "a");
    CHECK(rd.str(2, 0) == "x");
}

TEST_CASE("join - left semi/anti emit a left-only schema") {
    auto l = make_ks({1, 2, 3}, {"a", "b", "c"}, "k", "lv");
    auto r = make_ks({2, 2, 4}, {"x", "x2", "z"}, "k", "rv");
    SUBCASE("semi emits a matched left row once, no right columns") {
        auto out = join_ks(l, r, JoinType::LEFT_SEMI);
        REQUIRE(out.num_columns() == 2);  // k, lv only
        CHECK(name_of(out, 0) == "k");
        CHECK(name_of(out, 1) == "lv");
        Reader rd(out);
        REQUIRE(rd.rows() == 1);  // key 2 once despite two right matches
        CHECK(rd.i64(0, 0) == 2);
        CHECK(rd.str(1, 0) == "b");
    }
    SUBCASE("anti emits unmatched left rows, left-only") {
        auto out = join_ks(l, r, JoinType::LEFT_ANTI);
        REQUIRE(out.num_columns() == 2);
        Reader rd(out);
        REQUIRE(rd.rows() == 2);  // keys 1 and 3, ordered
        CHECK(rd.i64(0, 0) == 1);
        CHECK(rd.str(1, 0) == "a");
        CHECK(rd.i64(0, 1) == 3);
        CHECK(rd.str(1, 1) == "c");
    }
}

TEST_CASE("join - list<utf8> value passthrough") {
    RecordBatchBuilder lb;
    lb.declare_schema(
        {{"k", ColumnType::INT64}, {"tags", ColumnType::STRING_LIST}});
    lb.append_int64(0, 1);
    lb.append_string_list(1, {std::string_view("p"), std::string_view("q")});
    lb.end_row();
    lb.append_int64(0, 2);
    lb.append_string_list(1, {std::string_view("r")});
    lb.end_row();
    auto l = lb.finish();

    auto r = make_ks({1, 2}, {"x", "y"}, "k", "rv");
    auto out = join_ks(l, r, JoinType::INNER);
    Reader rd(out);
    REQUIRE(rd.rows() == 2);
    CHECK(rd.str_list(1, 0) == std::vector<std::string>{"p", "q"});
    CHECK(rd.str_list(1, 1) == std::vector<std::string>{"r"});
    CHECK(rd.str(2, 0) == "x");
}

TEST_CASE("join - list<int32> value passthrough normalizes to list<int64>") {
    auto l = make_k_i32list({1, 2}, {{10, 20}, {30}});
    auto r = make_ks({1, 2}, {"x", "y"}, "k", "rv");
    auto out = join_ks(l, r, JoinType::INNER);
    Reader rd(out);
    REQUIRE(rd.rows() == 2);
    // The narrow-int list survives with its values; the element widens to
    // int64.
    CHECK(rd.list_elem_type(1) == NANOARROW_TYPE_INT64);
    CHECK(rd.i64_list(1, 0) == std::vector<std::int64_t>{10, 20});
    CHECK(rd.i64_list(1, 1) == std::vector<std::int64_t>{30});
    CHECK(rd.str(2, 0) == "x");
}

namespace {

// {ts:INT64}
ArrowExportResult make_ts(const std::vector<std::int64_t>& ts) {
    RecordBatchBuilder b;
    b.declare_schema({{"ts", ColumnType::INT64}});
    for (std::int64_t t : ts) {
        b.append_int64(0, t);
        b.end_row();
    }
    return b.finish();
}

// {ts:INT64, val:STRING}; a nullopt ts becomes a null ts cell.
ArrowExportResult make_ts_vals(
    const std::vector<std::optional<std::int64_t>>& ts,
    const std::vector<std::string>& vals) {
    RecordBatchBuilder b;
    b.declare_schema({{"ts", ColumnType::INT64}, {"val", ColumnType::STRING}});
    for (std::size_t i = 0; i < ts.size(); ++i) {
        if (ts[i])
            b.append_int64(0, *ts[i]);
        else
            b.append_null(0);
        b.append_string(1, vals[i]);
        b.end_row();
    }
    return b.finish();
}

ArrowExportResult asof_g(ArrowExportResult& l, ArrowExportResult& r,
                         AsofDirection dir, bool has_tol = false,
                         std::int64_t tol = 0) {
    return asof_join(l.get_schema(), l.get_array(), 0, nullptr, r.get_schema(),
                     r.get_array(), 0, nullptr, 0, dir, has_tol, tol);
}

}  // namespace

TEST_CASE("asof - backward basic (n_equi=0)") {
    auto l = make_ts({3, 10, 20, 30});
    auto r = make_ts_vals({5, 15, 25}, {"a", "b", "c"});
    auto out = asof_g(l, r, AsofDirection::BACKWARD);
    REQUIRE(out.num_columns() == 2);
    CHECK(name_of(out, 0) == "ts");
    CHECK(name_of(out, 1) == "val");
    Reader rd(out);
    REQUIRE(rd.rows() == 4);
    CHECK((rd.i64(0, 0) == 3 && rd.null(1, 0)));  // before all right -> null
    CHECK((rd.i64(0, 1) == 10 && rd.str(1, 1) == "a"));
    CHECK((rd.i64(0, 2) == 20 && rd.str(1, 2) == "b"));
    CHECK((rd.i64(0, 3) == 30 && rd.str(1, 3) == "c"));
}

TEST_CASE("asof - forward") {
    auto l = make_ts({3, 10, 20, 30});
    auto r = make_ts_vals({5, 15, 25}, {"a", "b", "c"});
    auto out = asof_g(l, r, AsofDirection::FORWARD);
    Reader rd(out);
    REQUIRE(rd.rows() == 4);
    CHECK(rd.str(1, 0) == "a");  // 3 -> first ts >= 3 is 5
    CHECK(rd.str(1, 1) == "b");  // 10 -> 15
    CHECK(rd.str(1, 2) == "c");  // 20 -> 25
    CHECK(rd.null(1, 3));        // 30 -> none
}

TEST_CASE("asof - nearest with backward tie-break") {
    SUBCASE("closest wins") {
        auto l = make_ts({10, 20});
        auto r = make_ts_vals({8, 14, 25}, {"a", "b", "c"});
        auto out = asof_g(l, r, AsofDirection::NEAREST);
        Reader rd(out);
        REQUIRE(rd.rows() == 2);
        CHECK(rd.str(1, 0) == "a");  // 10: |8|=2 < |14|=4
        CHECK(rd.str(1, 1) == "c");  // 20: |25|=5 < |14|=6
    }
    SUBCASE("tie prefers backward") {
        auto l = make_ts({11});
        auto r = make_ts_vals({6, 16}, {"a", "b"});
        auto out = asof_g(l, r, AsofDirection::NEAREST);
        Reader rd(out);
        REQUIRE(rd.rows() == 1);
        CHECK(rd.str(1, 0) == "a");  // both dist 5 -> backward
    }
}

TEST_CASE("asof - equal right times follow merge_asof") {
    auto r = make_ts_vals({8, 8, 14, 14}, {"a", "b", "c", "d"});
    auto pick = [&](AsofDirection dir, std::int64_t t) {
        auto l = make_ts({t});
        auto out = asof_g(l, r, dir);
        Reader rd(out);
        REQUIRE(rd.rows() == 1);
        return rd.str(1, 0);
    };
    CHECK(pick(AsofDirection::BACKWARD, 8) == "b");
    CHECK(pick(AsofDirection::FORWARD, 8) == "a");
    CHECK(pick(AsofDirection::NEAREST, 8) == "b");
    CHECK(pick(AsofDirection::NEAREST, 14) == "d");
    CHECK(pick(AsofDirection::NEAREST, 10) == "b");
    CHECK(pick(AsofDirection::NEAREST, 11) == "b");
    CHECK(pick(AsofDirection::NEAREST, 12) == "c");
}

TEST_CASE("asof - equi-key partitioning") {
    RecordBatchBuilder lb;
    lb.declare_schema({{"pid", ColumnType::INT64}, {"ts", ColumnType::INT64}});
    auto addl = [&](std::int64_t p, std::int64_t t) {
        lb.append_int64(0, p);
        lb.append_int64(1, t);
        lb.end_row();
    };
    addl(1, 10);
    addl(2, 10);
    auto l = lb.finish();

    RecordBatchBuilder rb;
    rb.declare_schema({{"pid", ColumnType::INT64},
                       {"ts", ColumnType::INT64},
                       {"val", ColumnType::STRING}});
    auto addr = [&](std::int64_t p, std::int64_t t, const char* v) {
        rb.append_int64(0, p);
        rb.append_int64(1, t);
        rb.append_string(2, v);
        rb.end_row();
    };
    addr(1, 5, "a1");
    addr(1, 8, "a2");
    addr(2, 3, "b1");
    auto r = rb.finish();

    std::uint32_t le = 0, re = 0;  // equi col = pid (index 0)
    auto out =
        asof_join(l.get_schema(), l.get_array(), 1, &le, r.get_schema(),
                  r.get_array(), 1, &re, 1, AsofDirection::BACKWARD, false, 0);
    // Output: left pid, left ts, right val (right pid/ts are the lookup axis).
    REQUIRE(out.num_columns() == 3);
    CHECK(name_of(out, 2) == "val");
    Reader rd(out);
    REQUIRE(rd.rows() == 2);
    // Sorted by (pid, ts): pid 1 then pid 2.
    CHECK((rd.i64(0, 0) == 1 && rd.str(2, 0) == "a2"));  // pid1 ts10 -> ts8
    // pid2 ts10 matches only pid2's ts3, NOT pid1's nearer ts8.
    CHECK((rd.i64(0, 1) == 2 && rd.str(2, 1) == "b1"));
}

TEST_CASE("asof - tolerance") {
    auto l = make_ts({10});
    auto r = make_ts_vals({4}, {"a"});  // |10-4| = 6
    SUBCASE("just outside tol -> null") {
        auto out = asof_g(l, r, AsofDirection::BACKWARD, true, 5);
        Reader rd(out);
        CHECK(rd.null(1, 0));
    }
    SUBCASE("just inside tol -> match") {
        auto out = asof_g(l, r, AsofDirection::BACKWARD, true, 6);
        Reader rd(out);
        CHECK(rd.str(1, 0) == "a");
    }
}

TEST_CASE("asof - far-apart ts near int64 extremes (no signed overflow)") {
    const std::int64_t hi = std::numeric_limits<std::int64_t>::max();
    const std::int64_t lo = std::numeric_limits<std::int64_t>::min();
    SUBCASE("backward across the full int64 span, out of tol -> null") {
        auto l = make_ts({hi});
        auto r = make_ts_vals({lo}, {"a"});
        auto out = asof_g(l, r, AsofDirection::BACKWARD, true, 1000);
        Reader rd(out);
        REQUIRE(rd.rows() == 1);
        CHECK(rd.null(1, 0));
    }
    SUBCASE("nearest picks the closer extreme without overflow") {
        auto l = make_ts({0});
        auto r = make_ts_vals({lo, hi}, {"neg", "pos"});
        auto out = asof_g(l, r, AsofDirection::NEAREST);
        Reader rd(out);
        REQUIRE(rd.rows() == 1);
        // |0-INT64_MIN| = 2^63 > |0-INT64_MAX| = 2^63-1, so the max side wins.
        CHECK(rd.str(1, 0) == "pos");
    }
    SUBCASE("match near the top of the range") {
        auto l = make_ts({hi});
        auto r = make_ts_vals({hi - 5}, {"a"});
        auto out = asof_g(l, r, AsofDirection::BACKWARD, true, 5);
        Reader rd(out);
        CHECK(rd.str(1, 0) == "a");
    }
}

TEST_CASE("asof - float ts rejects a negative tolerance") {
    RecordBatchBuilder lb;
    lb.declare_schema({{"ts", ColumnType::DOUBLE}});
    lb.append_double(0, 10.0);
    lb.end_row();
    auto l = lb.finish();
    RecordBatchBuilder rb;
    rb.declare_schema(
        {{"ts", ColumnType::DOUBLE}, {"val", ColumnType::STRING}});
    rb.append_double(0, 10.0);
    rb.append_string(1, "a");
    rb.end_row();
    auto r = rb.finish();
    auto out = asof_join(l.get_schema(), l.get_array(), 0, nullptr,
                         r.get_schema(), r.get_array(), 0, nullptr, 0,
                         AsofDirection::BACKWARD, true, -1);
    Reader rd(out);
    REQUIRE(rd.rows() == 1);
    CHECK(rd.null(1, 0));  // exact ts but tol < 0 rejects
}

TEST_CASE("asof - null ts semantics") {
    SUBCASE("left null ts -> null right") {
        auto l = make_ts_vals({std::nullopt, 20}, {"L0", "L1"});
        auto r = make_ts_vals({5, 15}, {"a", "b"});
        // left ts col index 0, val col 1 passes through; right val looked up.
        auto out = asof_join(l.get_schema(), l.get_array(), 0, nullptr,
                             r.get_schema(), r.get_array(), 0, nullptr, 0,
                             AsofDirection::BACKWARD, false, 0);
        Reader rd(out);
        REQUIRE(rd.rows() == 2);
        // null-ts left sorts last; row order is [ts=20, ts=null].
        CHECK((rd.i64(0, 0) == 20 && rd.str(2, 0) == "b"));
        CHECK((rd.null(0, 1) && rd.null(2, 1)));  // null ts -> null right value
    }
    SUBCASE("right null ts is not a candidate") {
        auto l = make_ts({10});
        auto r = make_ts_vals({std::nullopt, 5}, {"skip", "a"});
        auto out = asof_g(l, r, AsofDirection::BACKWARD);
        Reader rd(out);
        CHECK(rd.str(1, 0) == "a");  // null-ts right ignored, matches ts 5
    }
}

TEST_CASE("asof - determinism across input order") {
    auto l1 = make_ts({10, 20, 30});
    auto r1 = make_ts_vals({5, 15, 25}, {"a", "b", "c"});
    auto l2 = make_ts({30, 10, 20});
    auto r2 = make_ts_vals({25, 5, 15}, {"c", "a", "b"});
    auto o1 = asof_g(l1, r1, AsofDirection::BACKWARD);
    auto o2 = asof_g(l2, r2, AsofDirection::BACKWARD);
    Reader a(o1), b(o2);
    REQUIRE(a.rows() == 3);
    REQUIRE(b.rows() == 3);
    for (std::int64_t i = 0; i < 3; ++i) {
        CHECK(a.i64(0, i) == b.i64(0, i));
        CHECK(a.str(1, i) == b.str(1, i));
    }
}

TEST_CASE("asof - list<utf8> value passthrough") {
    auto l = make_ts({10, 20});
    RecordBatchBuilder rb;
    rb.declare_schema(
        {{"ts", ColumnType::INT64}, {"tags", ColumnType::STRING_LIST}});
    rb.append_int64(0, 5);
    rb.append_string_list(1, {std::string_view("p"), std::string_view("q")});
    rb.end_row();
    rb.append_int64(0, 15);
    rb.append_string_list(1, {std::string_view("r")});
    rb.end_row();
    auto r = rb.finish();
    auto out = asof_g(l, r, AsofDirection::BACKWARD);
    Reader rd(out);
    REQUIRE(rd.rows() == 2);
    CHECK(rd.str_list(1, 0) == std::vector<std::string>{"p", "q"});
    CHECK(rd.str_list(1, 1) == std::vector<std::string>{"r"});
}

TEST_CASE("asof - allow_exact toggles an exact ts match") {
    auto l = make_ts({10});
    SUBCASE("backward default keeps the exact match") {
        auto r = make_ts_vals({5, 10}, {"older", "exact"});
        auto out = asof_g(l, r, AsofDirection::BACKWARD);
        Reader rd(out);
        CHECK(rd.str(1, 0) == "exact");
    }
    SUBCASE("backward allow_exact=false falls back to the next-older row") {
        auto r = make_ts_vals({5, 10}, {"older", "exact"});
        auto out = asof_join(l.get_schema(), l.get_array(), 0, nullptr,
                             r.get_schema(), r.get_array(), 0, nullptr, 0,
                             AsofDirection::BACKWARD, false, 0, false);
        Reader rd(out);
        CHECK(rd.str(1, 0) == "older");
    }
    SUBCASE("forward allow_exact=false takes the next-newer row") {
        auto r = make_ts_vals({10, 15}, {"exact", "newer"});
        auto out = asof_join(l.get_schema(), l.get_array(), 0, nullptr,
                             r.get_schema(), r.get_array(), 0, nullptr, 0,
                             AsofDirection::FORWARD, false, 0, false);
        Reader rd(out);
        CHECK(rd.str(1, 0) == "newer");
    }
}

namespace {

// {point:INT64}
ArrowExportResult make_points(const std::vector<std::int64_t>& pts) {
    RecordBatchBuilder b;
    b.declare_schema({{"point", ColumnType::INT64}});
    for (std::int64_t p : pts) {
        b.append_int64(0, p);
        b.end_row();
    }
    return b.finish();
}

// {lo:INT64, hi:INT64, val:STRING}
ArrowExportResult make_spans(const std::vector<std::int64_t>& lo,
                             const std::vector<std::int64_t>& hi,
                             const std::vector<std::string>& vals) {
    RecordBatchBuilder b;
    b.declare_schema({{"lo", ColumnType::INT64},
                      {"hi", ColumnType::INT64},
                      {"val", ColumnType::STRING}});
    for (std::size_t i = 0; i < lo.size(); ++i) {
        b.append_int64(0, lo[i]);
        b.append_int64(1, hi[i]);
        b.append_string(2, vals[i]);
        b.end_row();
    }
    return b.finish();
}

ArrowExportResult interval_g(ArrowExportResult& l, ArrowExportResult& r,
                             bool left_outer) {
    return interval_join(l.get_schema(), l.get_array(), 0, nullptr,
                         r.get_schema(), r.get_array(), 0, 1, nullptr, 0,
                         left_outer);
}

}  // namespace

TEST_CASE("interval - point in a single interval") {
    auto l = make_points({5, 15, 25});
    auto r = make_spans({0, 20}, {10, 30}, {"i0", "i1"});
    SUBCASE("schema: left cols then right non-lo/hi value cols") {
        auto out = interval_g(l, r, false);
        REQUIRE(out.num_columns() == 2);
        CHECK(name_of(out, 0) == "point");
        CHECK(name_of(out, 1) == "val");
    }
    SUBCASE("inner drops the uncovered point") {
        auto out = interval_g(l, r, false);
        Reader rd(out);
        REQUIRE(rd.rows() == 2);
        CHECK((rd.i64(0, 0) == 5 && rd.str(1, 0) == "i0"));
        CHECK((rd.i64(0, 1) == 25 && rd.str(1, 1) == "i1"));
    }
    SUBCASE("left_outer emits the uncovered point with null right") {
        auto out = interval_g(l, r, true);
        Reader rd(out);
        REQUIRE(rd.rows() == 3);
        CHECK((rd.i64(0, 0) == 5 && rd.str(1, 0) == "i0"));
        CHECK((rd.i64(0, 1) == 15 && rd.null(1, 1)));
        CHECK((rd.i64(0, 2) == 25 && rd.str(1, 2) == "i1"));
    }
}

TEST_CASE("interval - nested/overlapping emit one row per interval") {
    auto l = make_points({5});
    auto r = make_spans({0, 2}, {10, 8}, {"outer", "inner"});
    auto out = interval_g(l, r, false);
    Reader rd(out);
    REQUIRE(rd.rows() == 2);
    // Deterministic order by (lo, hi, row): (0,10) before (2,8).
    CHECK((rd.i64(0, 0) == 5 && rd.str(1, 0) == "outer"));
    CHECK((rd.i64(0, 1) == 5 && rd.str(1, 1) == "inner"));
}

TEST_CASE("interval - closed boundaries match at lo and hi") {
    auto l = make_points({0, 10});
    auto r = make_spans({0}, {10}, {"span"});
    auto out = interval_g(l, r, false);
    Reader rd(out);
    REQUIRE(rd.rows() == 2);
    CHECK((rd.i64(0, 0) == 0 && rd.str(1, 0) == "span"));   // point == lo
    CHECK((rd.i64(0, 1) == 10 && rd.str(1, 1) == "span"));  // point == hi
}

TEST_CASE("interval - equi-key partitioning") {
    RecordBatchBuilder lb;
    lb.declare_schema(
        {{"pid", ColumnType::INT64}, {"point", ColumnType::INT64}});
    auto addl = [&](std::int64_t p, std::int64_t t) {
        lb.append_int64(0, p);
        lb.append_int64(1, t);
        lb.end_row();
    };
    addl(1, 5);
    addl(2, 5);
    auto l = lb.finish();

    RecordBatchBuilder rb;
    rb.declare_schema({{"pid", ColumnType::INT64},
                       {"lo", ColumnType::INT64},
                       {"hi", ColumnType::INT64},
                       {"val", ColumnType::STRING}});
    auto addr = [&](std::int64_t p, std::int64_t lo, std::int64_t hi,
                    const char* v) {
        rb.append_int64(0, p);
        rb.append_int64(1, lo);
        rb.append_int64(2, hi);
        rb.append_string(3, v);
        rb.end_row();
    };
    addr(1, 0, 10, "p1");          // would contain point 5 in pid 1
    addr(2, 0, 10, "p2");          // would contain point 5 in pid 2
    auto r = rb.finish();

    std::uint32_t le = 0, re = 0;  // equi col = pid
    auto out =
        interval_join(l.get_schema(), l.get_array(), 1, &le, r.get_schema(),
                      r.get_array(), 1, 2, &re, 1, false);
    // Output: left pid, left point, right val (right pid/lo/hi are the axis).
    REQUIRE(out.num_columns() == 3);
    CHECK(name_of(out, 2) == "val");
    Reader rd(out);
    REQUIRE(rd.rows() == 2);
    // Each point matches only its own partition's interval.
    CHECK((rd.i64(0, 0) == 1 && rd.str(2, 0) == "p1"));
    CHECK((rd.i64(0, 1) == 2 && rd.str(2, 1) == "p2"));
}

TEST_CASE("interval - determinism across input order") {
    auto l1 = make_points({5, 25});
    auto r1 = make_spans({0, 20}, {10, 30}, {"i0", "i1"});
    auto l2 = make_points({25, 5});
    auto r2 = make_spans({20, 0}, {30, 10}, {"i1", "i0"});
    auto o1 = interval_g(l1, r1, true);
    auto o2 = interval_g(l2, r2, true);
    Reader a(o1), b(o2);
    REQUIRE(a.rows() == 2);
    REQUIRE(b.rows() == 2);
    for (std::int64_t i = 0; i < 2; ++i) {
        CHECK(a.i64(0, i) == b.i64(0, i));
        CHECK(a.str(1, i) == b.str(1, i));
    }
}

TEST_CASE("interval - list<utf8> value passthrough") {
    auto l = make_points({5});
    RecordBatchBuilder rb;
    rb.declare_schema({{"lo", ColumnType::INT64},
                       {"hi", ColumnType::INT64},
                       {"tags", ColumnType::STRING_LIST}});
    rb.append_int64(0, 0);
    rb.append_int64(1, 10);
    rb.append_string_list(2, {std::string_view("p"), std::string_view("q")});
    rb.end_row();
    auto r = rb.finish();
    auto out =
        interval_join(l.get_schema(), l.get_array(), 0, nullptr, r.get_schema(),
                      r.get_array(), 0, 1, nullptr, 0, false);
    Reader rd(out);
    REQUIRE(rd.rows() == 1);
    CHECK(rd.str_list(1, 0) == std::vector<std::string>{"p", "q"});
}

TEST_CASE("interval - right_outer emits an interval covering no point") {
    auto l = make_points({5});
    // (0,10) covers point 5; (20,30) covers nothing.
    auto r = make_spans({0, 20}, {10, 30}, {"hit", "miss"});
    SUBCASE("off: the unmatched right interval is dropped") {
        auto out = interval_g(l, r, false);
        Reader rd(out);
        REQUIRE(rd.rows() == 1);
        CHECK((rd.i64(0, 0) == 5 && rd.str(1, 0) == "hit"));
    }
    SUBCASE("on: the unmatched right interval is emitted null-left") {
        auto out =
            interval_join(l.get_schema(), l.get_array(), 0, nullptr,
                          r.get_schema(), r.get_array(), 0, 1, nullptr, 0,
                          /*left_outer=*/false, /*right_outer=*/true);
        Reader rd(out);
        REQUIRE(rd.rows() == 2);
        CHECK((rd.i64(0, 0) == 5 && rd.str(1, 0) == "hit"));
        CHECK((rd.null(0, 1) && rd.str(1, 1) == "miss"));
    }
}

TEST_CASE("interval - null semantics") {
    SUBCASE("null point matches nothing") {
        auto l = make_ts_vals({std::nullopt, 5}, {"L0", "L1"});
        auto r = make_spans({0}, {10}, {"span"});
        // left point col 0, val col 1 passes through.
        auto out = interval_join(l.get_schema(), l.get_array(), 0, nullptr,
                                 r.get_schema(), r.get_array(), 0, 1, nullptr,
                                 0, true);
        Reader rd(out);
        REQUIRE(rd.rows() == 2);
        // null-point left sorts last; row order is [point=5, point=null].
        CHECK((rd.i64(0, 0) == 5 && rd.str(2, 0) == "span"));
        CHECK((rd.null(0, 1) && rd.null(2, 1)));
    }
    SUBCASE("null lo interval is never active") {
        auto l = make_points({5});
        RecordBatchBuilder rb;
        rb.declare_schema({{"lo", ColumnType::INT64},
                           {"hi", ColumnType::INT64},
                           {"val", ColumnType::STRING}});
        rb.append_null(0);  // null lo
        rb.append_int64(1, 10);
        rb.append_string(2, "skip");
        rb.end_row();
        auto r = rb.finish();
        auto out = interval_g(l, r, false);
        Reader rd(out);
        REQUIRE(rd.rows() == 0);
    }
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW
