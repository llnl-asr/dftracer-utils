// Every "dftu.lazy.*" registry entry, resolved by name and run through
// dftu_op_run_lazy, must collect to the same result as calling the underlying
// dftu_lazyframe_* ABI function directly - proving the .def row's tokens and
// the runner's argument marshalling agree.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/op.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

using dftracer::utils::dataframe::OpArgs;

namespace {

dftu_series* i64_col(const std::int64_t* v, std::int64_t n) {
    return dftu_series_new_flat(DFTU_TYPE_INT64, v, n, nullptr);
}

const std::int64_t* i64_of(const dftu_series* c) {
    return static_cast<const std::int64_t*>(dftu_series_data(c));
}

dftu_scalar i64_scalar(std::int64_t v) {
    dftu_scalar s;
    s.kind = DFTU_SCALAR_TAG_I64;
    s.value.i = v;
    return s;
}

// A two-column frame: "k" keys, "v" values.
dftu_dataframe* make_frame(const std::vector<std::int64_t>& k,
                           const std::vector<std::int64_t>& v) {
    dftu_series* cols[2] = {
        i64_col(k.data(), static_cast<std::int64_t>(k.size())),
        i64_col(v.data(), static_cast<std::int64_t>(v.size()))};
    const char* names[2] = {"k", "v"};
    return dftu_dataframe_new(names, cols, 2);
}

// Row/column-count and, for every INT64 column present in both, elementwise
// equality. Sufficient to prove the registry path reached the same function
// with the same arguments as the direct call: on a mismatch either the shape
// or the data would disagree.
void check_same(dftu_dataframe* a, dftu_dataframe* b) {
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(dftu_dataframe_num_rows(a) == dftu_dataframe_num_rows(b));
    CHECK(dftu_dataframe_num_columns(a) == dftu_dataframe_num_columns(b));
    std::int32_t nc = dftu_dataframe_num_columns(a);
    for (std::int32_t i = 0; i < nc; ++i) {
        const char* name = dftu_dataframe_column_name(a, i);
        dftu_series* ca = dftu_dataframe_column(a, name);
        dftu_series* cb = dftu_dataframe_column(b, name);
        REQUIRE(ca != nullptr);
        REQUIRE(cb != nullptr);
        CHECK(dftu_series_type(ca) == dftu_series_type(cb));
        CHECK(dftu_series_length(ca) == dftu_series_length(cb));
        if (dftu_series_type(ca) == DFTU_TYPE_INT64 &&
            dftu_series_type(cb) == DFTU_TYPE_INT64) {
            const std::int64_t* da = i64_of(ca);
            const std::int64_t* db = i64_of(cb);
            std::int64_t n = dftu_series_length(ca);
            for (std::int64_t r = 0; r < n; ++r) CHECK(da[r] == db[r]);
        }
        dftu_series_free(ca);
        dftu_series_free(cb);
    }
}

void check_kind_and_arity(const char* name, std::uint32_t expect_arity) {
    const dftu_op_desc* op = dftu_op_find(name);
    REQUIRE(op != nullptr);
    CHECK(dftu_op_kind_of(op->sig) == DFTU_OP_KIND_LAZY);
    CHECK(dftu_op_arity(op->sig) == expect_arity);
}

// Runs `name` through the registry against `lf1` and directly via `direct`
// against `lf2`, collects both, and checks they agree.
dftu_dataframe* run_registry(const char* name, const dftu_lazyframe* lf,
                             const dftu_op_arg* a) {
    const dftu_op_desc* op = dftu_op_find(name);
    REQUIRE(op != nullptr);
    const dftu_lazyframe* in[1] = {lf};
    dftu_lazyframe* out = dftu_op_run_lazy(op, in, 1, a);
    REQUIRE(out != nullptr);
    dftu_dataframe* collected = dftu_lazyframe_collect(out, 0);
    dftu_lazyframe_free(out);
    return collected;
}

}  // namespace

TEST_SUITE("lazy_ops") {
    TEST_CASE("no-operand lazy ops match their direct ABI call") {
        struct Case {
            const char* name;
            dftu_lazyframe* (*fn)(const dftu_lazyframe*);
        };
        const Case cases[] = {
            {"dftu.lazy.auto_spill", dftu_lazyframe_auto_spill},
            {"dftu.lazy.describe", dftu_lazyframe_describe},
            {"dftu.lazy.drop_duplicates", dftu_lazyframe_drop_duplicates},
            {"dftu.lazy.drop_nulls", dftu_lazyframe_drop_nulls},
            {"dftu.lazy.is_duplicated", dftu_lazyframe_is_duplicated},
            {"dftu.lazy.is_unique", dftu_lazyframe_is_unique},
            {"dftu.lazy.null_count", dftu_lazyframe_null_count},
            {"dftu.lazy.unique", dftu_lazyframe_unique},
            {"dftu.lazy.reverse", dftu_lazyframe_reverse},
        };
        for (const Case& c : cases) {
            INFO(c.name);
            check_kind_and_arity(c.name, 1);
            dftu_dataframe* df = make_frame({1, 1, 2}, {5, 5, 9});
            REQUIRE(df != nullptr);
            dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
            dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);

            dftu_dataframe* via_registry = run_registry(c.name, lf1, nullptr);
            dftu_lazyframe* direct = c.fn(lf2);
            REQUIRE(direct != nullptr);
            dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);

            check_same(via_registry, via_direct);

            dftu_dataframe_free(via_registry);
            dftu_dataframe_free(via_direct);
            dftu_lazyframe_free(direct);
            dftu_lazyframe_free(lf1);
            dftu_lazyframe_free(lf2);
            dftu_dataframe_free(df);
        }
    }

    TEST_CASE("STR-operand lazy ops match their direct ABI call") {
        dftu_dataframe* df = make_frame({1, 2}, {10, 20});
        REQUIRE(df != nullptr);

        {
            check_kind_and_arity("dftu.lazy.with_row_index", 1);
            dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
            dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
            OpArgs a;
            a.str(1, "idx");
            dftu_dataframe* via_registry =
                run_registry("dftu.lazy.with_row_index", lf1, a);
            dftu_lazyframe* direct = dftu_lazyframe_with_row_index(lf2, "idx");
            REQUIRE(direct != nullptr);
            dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
            check_same(via_registry, via_direct);
            dftu_dataframe_free(via_registry);
            dftu_dataframe_free(via_direct);
            dftu_lazyframe_free(direct);
            dftu_lazyframe_free(lf1);
            dftu_lazyframe_free(lf2);
        }
        {
            check_kind_and_arity("dftu.lazy.to_dummies", 1);
            dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
            dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
            OpArgs a;
            a.str(1, "k");
            dftu_dataframe* via_registry =
                run_registry("dftu.lazy.to_dummies", lf1, a);
            dftu_lazyframe* direct = dftu_lazyframe_to_dummies(lf2, "k");
            REQUIRE(direct != nullptr);
            dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
            check_same(via_registry, via_direct);
            dftu_dataframe_free(via_registry);
            dftu_dataframe_free(via_direct);
            dftu_lazyframe_free(direct);
            dftu_lazyframe_free(lf1);
            dftu_lazyframe_free(lf2);
        }

        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu.lazy.explode matches dftu_lazyframe_explode") {
        std::int32_t offs[3] = {0, 2, 4};
        std::int64_t items[4] = {1, 2, 3, 4};
        dftu_series* item_col = i64_col(items, 4);
        dftu_series* list_col = dftu_series_new_list(offs, 2, item_col);
        REQUIRE(list_col != nullptr);
        dftu_series* cols[1] = {list_col};
        const char* names[1] = {"xs"};
        dftu_dataframe* df = dftu_dataframe_new(names, cols, 1);
        REQUIRE(df != nullptr);

        check_kind_and_arity("dftu.lazy.explode", 1);
        dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
        dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
        OpArgs a;
        a.str(1, "xs");
        dftu_dataframe* via_registry =
            run_registry("dftu.lazy.explode", lf1, a);
        dftu_lazyframe* direct = dftu_lazyframe_explode(lf2, "xs");
        REQUIRE(direct != nullptr);
        dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
        check_same(via_registry, via_direct);
        CHECK(dftu_dataframe_num_rows(via_registry) == 4);

        dftu_dataframe_free(via_registry);
        dftu_dataframe_free(via_direct);
        dftu_lazyframe_free(direct);
        dftu_lazyframe_free(lf1);
        dftu_lazyframe_free(lf2);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu.lazy.fill_null matches dftu_lazyframe_fill_null") {
        std::int64_t k[3] = {1, 2, 3};
        std::int64_t v[3] = {10, 0, 30};
        dftu_series* kc = i64_col(k, 3);
        std::uint8_t validity = 0b101;  // row 1 is null
        dftu_series* vc =
            dftu_series_new_flat(DFTU_TYPE_INT64, v, 3, &validity);
        REQUIRE(vc != nullptr);
        dftu_series* cols[2] = {kc, vc};
        const char* names[2] = {"k", "v"};
        dftu_dataframe* df = dftu_dataframe_new(names, cols, 2);
        REQUIRE(df != nullptr);

        check_kind_and_arity("dftu.lazy.fill_null", 1);
        dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
        dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
        OpArgs a;
        a.scalar(1, i64_scalar(-1));
        dftu_dataframe* via_registry =
            run_registry("dftu.lazy.fill_null", lf1, a);
        dftu_lazyframe* direct = dftu_lazyframe_fill_null(lf2, i64_scalar(-1));
        REQUIRE(direct != nullptr);
        dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
        check_same(via_registry, via_direct);

        dftu_dataframe_free(via_registry);
        dftu_dataframe_free(via_direct);
        dftu_lazyframe_free(direct);
        dftu_lazyframe_free(lf1);
        dftu_lazyframe_free(lf2);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu.lazy.filter matches dftu_lazyframe_filter") {
        dftu_dataframe* df = make_frame({1, 1, 2, 3}, {5, 15, 20, 8});
        REQUIRE(df != nullptr);
        dftu_expr* col1 = dftu_expr_col(1);
        dftu_expr* pred = dftu_expr_cmp(DFTU_CMP_GT, col1, i64_scalar(10));
        dftu_expr_free(col1);  // a child is shared, the handle is the caller's
        REQUIRE(pred != nullptr);

        check_kind_and_arity("dftu.lazy.filter", 1);
        dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
        dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
        OpArgs a;
        a.expr(1, pred);
        dftu_dataframe* via_registry = run_registry("dftu.lazy.filter", lf1, a);
        dftu_lazyframe* direct = dftu_lazyframe_filter(lf2, pred);
        REQUIRE(direct != nullptr);
        dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
        check_same(via_registry, via_direct);
        CHECK(dftu_dataframe_num_rows(via_registry) == 2);

        dftu_dataframe_free(via_registry);
        dftu_dataframe_free(via_direct);
        dftu_lazyframe_free(direct);
        dftu_lazyframe_free(lf1);
        dftu_lazyframe_free(lf2);
        dftu_expr_free(pred);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu.lazy.with_column matches dftu_lazyframe_with_column") {
        dftu_dataframe* df = make_frame({1, 2, 3}, {10, 20, 30});
        REQUIRE(df != nullptr);
        dftu_expr* col1 = dftu_expr_col(1);
        dftu_expr* one = dftu_expr_lit_i64(1);
        dftu_expr* expr = dftu_expr_binary(0 /* BinaryOp::Add */, col1, one);
        dftu_expr_free(col1);
        dftu_expr_free(one);
        REQUIRE(expr != nullptr);

        check_kind_and_arity("dftu.lazy.with_column", 1);
        dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
        dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
        OpArgs a;
        a.str(1, "v_plus_1");
        a.expr(2, expr);
        dftu_dataframe* via_registry =
            run_registry("dftu.lazy.with_column", lf1, a);
        dftu_lazyframe* direct =
            dftu_lazyframe_with_column(lf2, "v_plus_1", expr);
        REQUIRE(direct != nullptr);
        dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
        check_same(via_registry, via_direct);

        dftu_series* added = dftu_dataframe_column(via_registry, "v_plus_1");
        REQUIRE(added != nullptr);
        CHECK(i64_of(added)[0] == 11);
        dftu_series_free(added);

        dftu_dataframe_free(via_registry);
        dftu_dataframe_free(via_direct);
        dftu_lazyframe_free(direct);
        dftu_lazyframe_free(lf1);
        dftu_lazyframe_free(lf2);
        dftu_expr_free(expr);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu.lazy.head / dftu.lazy.tail match their direct ABI call") {
        dftu_dataframe* df = make_frame({1, 2, 3, 4}, {10, 20, 30, 40});
        REQUIRE(df != nullptr);

        {
            check_kind_and_arity("dftu.lazy.head", 1);
            dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
            dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
            OpArgs a;
            a.i64(1, 2);
            dftu_dataframe* via_registry =
                run_registry("dftu.lazy.head", lf1, a);
            dftu_lazyframe* direct = dftu_lazyframe_head(lf2, 2);
            REQUIRE(direct != nullptr);
            dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
            check_same(via_registry, via_direct);
            CHECK(dftu_dataframe_num_rows(via_registry) == 2);
            dftu_dataframe_free(via_registry);
            dftu_dataframe_free(via_direct);
            dftu_lazyframe_free(direct);
            dftu_lazyframe_free(lf1);
            dftu_lazyframe_free(lf2);
        }
        {
            check_kind_and_arity("dftu.lazy.tail", 1);
            dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
            dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
            OpArgs a;
            a.i64(1, 2);
            dftu_dataframe* via_registry =
                run_registry("dftu.lazy.tail", lf1, a);
            dftu_lazyframe* direct = dftu_lazyframe_tail(lf2, 2);
            REQUIRE(direct != nullptr);
            dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
            check_same(via_registry, via_direct);
            CHECK(dftu_dataframe_num_rows(via_registry) == 2);
            dftu_dataframe_free(via_registry);
            dftu_dataframe_free(via_direct);
            dftu_lazyframe_free(direct);
            dftu_lazyframe_free(lf1);
            dftu_lazyframe_free(lf2);
        }

        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu.lazy.memory_budget matches dftu_lazyframe_memory_budget") {
        dftu_dataframe* df = make_frame({1, 2, 3}, {10, 20, 30});
        REQUIRE(df != nullptr);

        check_kind_and_arity("dftu.lazy.memory_budget", 1);
        dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
        dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
        OpArgs a;
        a.i64(1, 1024);
        dftu_dataframe* via_registry =
            run_registry("dftu.lazy.memory_budget", lf1, a);
        dftu_lazyframe* direct = dftu_lazyframe_memory_budget(lf2, 1024);
        REQUIRE(direct != nullptr);
        dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
        check_same(via_registry, via_direct);

        dftu_dataframe_free(via_registry);
        dftu_dataframe_free(via_direct);
        dftu_lazyframe_free(direct);
        dftu_lazyframe_free(lf1);
        dftu_lazyframe_free(lf2);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu.lazy.slice matches dftu_lazyframe_slice") {
        dftu_dataframe* df = make_frame({1, 2, 3, 4}, {10, 20, 30, 40});
        REQUIRE(df != nullptr);

        check_kind_and_arity("dftu.lazy.slice", 1);
        dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
        dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
        OpArgs a;
        a.i64(1, 1).i64(2, 2);
        dftu_dataframe* via_registry = run_registry("dftu.lazy.slice", lf1, a);
        dftu_lazyframe* direct = dftu_lazyframe_slice(lf2, 1, 2);
        REQUIRE(direct != nullptr);
        dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
        check_same(via_registry, via_direct);
        CHECK(dftu_dataframe_num_rows(via_registry) == 2);

        dftu_dataframe_free(via_registry);
        dftu_dataframe_free(via_direct);
        dftu_lazyframe_free(direct);
        dftu_lazyframe_free(lf1);
        dftu_lazyframe_free(lf2);
        dftu_dataframe_free(df);
    }

    TEST_CASE(
        "dftu.lazy.sample matches dftu_lazyframe_sample for a shared seed") {
        dftu_dataframe* df = make_frame({1, 2, 3, 4, 5}, {1, 2, 3, 4, 5});
        REQUIRE(df != nullptr);

        check_kind_and_arity("dftu.lazy.sample", 1);
        dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
        dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
        OpArgs a;
        a.i64(1, 2).i64(2, 42);
        dftu_dataframe* via_registry = run_registry("dftu.lazy.sample", lf1, a);
        dftu_lazyframe* direct = dftu_lazyframe_sample(lf2, 2, 42);
        REQUIRE(direct != nullptr);
        dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
        check_same(via_registry, via_direct);
        CHECK(dftu_dataframe_num_rows(via_registry) == 2);

        dftu_dataframe_free(via_registry);
        dftu_dataframe_free(via_direct);
        dftu_lazyframe_free(direct);
        dftu_lazyframe_free(lf1);
        dftu_lazyframe_free(lf2);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu.lazy.select / dftu.lazy.rename match their direct call") {
        dftu_dataframe* df = make_frame({1, 2}, {10, 20});
        REQUIRE(df != nullptr);

        {
            check_kind_and_arity("dftu.lazy.select", 1);
            dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
            dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
            const char* names[1] = {"v"};
            OpArgs a;
            a.strlist(1, names, 1);
            dftu_dataframe* via_registry =
                run_registry("dftu.lazy.select", lf1, a);
            dftu_lazyframe* direct = dftu_lazyframe_select(lf2, names, 1);
            REQUIRE(direct != nullptr);
            dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
            check_same(via_registry, via_direct);
            CHECK(dftu_dataframe_num_columns(via_registry) == 1);
            dftu_dataframe_free(via_registry);
            dftu_dataframe_free(via_direct);
            dftu_lazyframe_free(direct);
            dftu_lazyframe_free(lf1);
            dftu_lazyframe_free(lf2);
        }
        {
            check_kind_and_arity("dftu.lazy.rename", 1);
            dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
            dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
            const char* names[2] = {"key", "val"};
            OpArgs a;
            a.strlist(1, names, 2);
            dftu_dataframe* via_registry =
                run_registry("dftu.lazy.rename", lf1, a);
            dftu_lazyframe* direct = dftu_lazyframe_rename(lf2, names, 2);
            REQUIRE(direct != nullptr);
            dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
            check_same(via_registry, via_direct);
            CHECK(dftu_dataframe_column_name(via_registry, 0) ==
                  std::string("key"));
            dftu_dataframe_free(via_registry);
            dftu_dataframe_free(via_direct);
            dftu_lazyframe_free(direct);
            dftu_lazyframe_free(lf1);
            dftu_lazyframe_free(lf2);
        }

        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu.lazy.unpivot / dftu.lazy.melt match their direct call") {
        dftu_dataframe* df = make_frame({1, 2}, {10, 20});
        REQUIRE(df != nullptr);
        const char* id_vars[1] = {"k"};
        const char* value_vars[1] = {"v"};

        for (const char* name : {"dftu.lazy.unpivot", "dftu.lazy.melt"}) {
            INFO(name);
            check_kind_and_arity(name, 1);
            dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
            dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
            OpArgs a;
            a.strlist(1, id_vars, 1);
            a.strlist(2, value_vars, 1);
            dftu_dataframe* via_registry = run_registry(name, lf1, a);
            dftu_lazyframe* direct =
                std::string(name) == "dftu.lazy.unpivot"
                    ? dftu_lazyframe_unpivot(lf2, id_vars, 1, value_vars, 1)
                    : dftu_lazyframe_melt(lf2, id_vars, 1, value_vars, 1);
            REQUIRE(direct != nullptr);
            dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
            check_same(via_registry, via_direct);
            dftu_dataframe_free(via_registry);
            dftu_dataframe_free(via_direct);
            dftu_lazyframe_free(direct);
            dftu_lazyframe_free(lf1);
            dftu_lazyframe_free(lf2);
        }

        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu.lazy.group_by matches dftu_lazyframe_group_by") {
        dftu_dataframe* df = make_frame({1, 1, 2, 3}, {5, 15, 20, 8});
        REQUIRE(df != nullptr);
        const char* keys[1] = {"k"};
        dftu_group_agg aggs[1] = {{"sum", "v", "v_sum"}};

        check_kind_and_arity("dftu.lazy.group_by", 1);
        dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
        dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
        OpArgs a;
        a.strlist(1, keys, 1);
        a.agglist(2, {aggs, 1});
        dftu_dataframe* grouped_registry =
            run_registry("dftu.lazy.group_by", lf1, a);
        dftu_lazyframe* grouped_direct_lf =
            dftu_lazyframe_group_by(lf2, keys, 1, aggs, 1);
        REQUIRE(grouped_direct_lf != nullptr);
        dftu_dataframe* grouped_direct =
            dftu_lazyframe_collect(grouped_direct_lf, 0);

        // group_by's output row order is not guaranteed, so sort both sides by
        // "k" before comparing.
        dftu_lazyframe* sr = dftu_dataframe_lazy(grouped_registry);
        dftu_lazyframe* sd = dftu_dataframe_lazy(grouped_direct);
        dftu_lazyframe* sr_sorted = dftu_lazyframe_sort_by(sr, "k", 0);
        dftu_lazyframe* sd_sorted = dftu_lazyframe_sort_by(sd, "k", 0);
        dftu_dataframe* out_r = dftu_lazyframe_collect(sr_sorted, 0);
        dftu_dataframe* out_d = dftu_lazyframe_collect(sd_sorted, 0);
        check_same(out_r, out_d);
        CHECK(dftu_dataframe_num_rows(out_r) == 3);

        dftu_dataframe_free(out_r);
        dftu_dataframe_free(out_d);
        dftu_lazyframe_free(sr_sorted);
        dftu_lazyframe_free(sd_sorted);
        dftu_lazyframe_free(sr);
        dftu_lazyframe_free(sd);
        dftu_dataframe_free(grouped_registry);
        dftu_dataframe_free(grouped_direct);
        dftu_lazyframe_free(grouped_direct_lf);
        dftu_lazyframe_free(lf1);
        dftu_lazyframe_free(lf2);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu.lazy.sort_by matches dftu_lazyframe_sort_by") {
        dftu_dataframe* df = make_frame({3, 1, 2}, {30, 10, 20});
        REQUIRE(df != nullptr);

        check_kind_and_arity("dftu.lazy.sort_by", 1);
        dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
        dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
        OpArgs a;
        a.str(1, "k").i32(2, 0);
        dftu_dataframe* via_registry =
            run_registry("dftu.lazy.sort_by", lf1, a);
        dftu_lazyframe* direct = dftu_lazyframe_sort_by(lf2, "k", 0);
        REQUIRE(direct != nullptr);
        dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
        check_same(via_registry, via_direct);
        dftu_series* kc = dftu_dataframe_column(via_registry, "k");
        REQUIRE(kc != nullptr);
        CHECK(i64_of(kc)[0] == 1);
        dftu_series_free(kc);

        dftu_dataframe_free(via_registry);
        dftu_dataframe_free(via_direct);
        dftu_lazyframe_free(direct);
        dftu_lazyframe_free(lf1);
        dftu_lazyframe_free(lf2);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu.lazy.topk matches dftu_lazyframe_topk") {
        dftu_dataframe* df = make_frame({1, 2, 3, 4}, {40, 10, 30, 20});
        REQUIRE(df != nullptr);

        check_kind_and_arity("dftu.lazy.topk", 1);
        dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
        dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
        OpArgs a;
        a.str(1, "v").i64(2, 2).i32(3, 1);
        dftu_dataframe* via_registry = run_registry("dftu.lazy.topk", lf1, a);
        dftu_lazyframe* direct = dftu_lazyframe_topk(lf2, "v", 2, 1);
        REQUIRE(direct != nullptr);
        dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
        check_same(via_registry, via_direct);
        CHECK(dftu_dataframe_num_rows(via_registry) == 2);

        dftu_dataframe_free(via_registry);
        dftu_dataframe_free(via_direct);
        dftu_lazyframe_free(direct);
        dftu_lazyframe_free(lf1);
        dftu_lazyframe_free(lf2);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu.lazy.pivot matches dftu_lazyframe_pivot") {
        dftu_dataframe* df = make_frame({1, 2}, {10, 20});
        REQUIRE(df != nullptr);

        check_kind_and_arity("dftu.lazy.pivot", 1);
        dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
        dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
        OpArgs a;
        a.str(1, "k").str(2, "k").str(3, "v").str(4, "sum");
        dftu_dataframe* via_registry = run_registry("dftu.lazy.pivot", lf1, a);
        dftu_lazyframe* direct =
            dftu_lazyframe_pivot(lf2, "k", "k", "v", "sum");
        REQUIRE(direct != nullptr);
        dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
        check_same(via_registry, via_direct);

        dftu_dataframe_free(via_registry);
        dftu_dataframe_free(via_direct);
        dftu_lazyframe_free(direct);
        dftu_lazyframe_free(lf1);
        dftu_lazyframe_free(lf2);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu.lazy.filter_mask matches dftu_lazyframe_filter_mask") {
        dftu_dataframe* df = make_frame({1, 2, 3}, {10, 20, 30});
        REQUIRE(df != nullptr);

        check_kind_and_arity("dftu.lazy.filter_mask", 1);
        dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
        dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
        const std::uint8_t mask_bits = 0b101;  // rows 0 and 2 set.
        dftu_series* mask =
            dftu_series_new_flat(DFTU_TYPE_BOOL, &mask_bits, 3, nullptr);
        REQUIRE(mask != nullptr);
        OpArgs a;
        a.series(1, mask);
        dftu_dataframe* via_registry =
            run_registry("dftu.lazy.filter_mask", lf1, a);
        dftu_lazyframe* direct = dftu_lazyframe_filter_mask(lf2, mask);
        REQUIRE(direct != nullptr);
        dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
        check_same(via_registry, via_direct);
        CHECK(dftu_dataframe_num_rows(via_registry) == 2);

        dftu_series_free(mask);
        dftu_dataframe_free(via_registry);
        dftu_dataframe_free(via_direct);
        dftu_lazyframe_free(direct);
        dftu_lazyframe_free(lf1);
        dftu_lazyframe_free(lf2);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu.lazy.sort_by_multi matches dftu_lazyframe_sort_by_multi") {
        dftu_dataframe* df = make_frame({2, 1, 1}, {20, 30, 10});
        REQUIRE(df != nullptr);

        check_kind_and_arity("dftu.lazy.sort_by_multi", 1);
        dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
        dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
        const char* by[2] = {"k", "v"};
        OpArgs a;
        a.strlist(1, by, 2).i32(2, 0);
        dftu_dataframe* via_registry =
            run_registry("dftu.lazy.sort_by_multi", lf1, a);
        dftu_lazyframe* direct = dftu_lazyframe_sort_by_multi(lf2, by, 2, 0);
        REQUIRE(direct != nullptr);
        dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
        check_same(via_registry, via_direct);
        dftu_series* kc = dftu_dataframe_column(via_registry, "k");
        dftu_series* vc = dftu_dataframe_column(via_registry, "v");
        REQUIRE(kc != nullptr);
        REQUIRE(vc != nullptr);
        CHECK(i64_of(kc)[0] == 1);
        CHECK(i64_of(vc)[0] == 10);
        dftu_series_free(kc);
        dftu_series_free(vc);

        dftu_dataframe_free(via_registry);
        dftu_dataframe_free(via_direct);
        dftu_lazyframe_free(direct);
        dftu_lazyframe_free(lf1);
        dftu_lazyframe_free(lf2);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu.lazy.take matches dftu_lazyframe_take") {
        dftu_dataframe* df = make_frame({1, 2, 3, 4}, {10, 20, 30, 40});
        REQUIRE(df != nullptr);

        check_kind_and_arity("dftu.lazy.take", 1);
        dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
        dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
        const std::int64_t idx[3] = {2, 0, 0};
        OpArgs a;
        a.i64list(1, idx);
        dftu_dataframe* via_registry = run_registry("dftu.lazy.take", lf1, a);
        dftu_lazyframe* direct = dftu_lazyframe_take(lf2, idx, 3);
        REQUIRE(direct != nullptr);
        dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
        check_same(via_registry, via_direct);
        CHECK(dftu_dataframe_num_rows(via_registry) == 3);
        dftu_series* kc = dftu_dataframe_column(via_registry, "k");
        REQUIRE(kc != nullptr);
        CHECK(i64_of(kc)[0] == 3);
        CHECK(i64_of(kc)[1] == 1);
        CHECK(i64_of(kc)[2] == 1);
        dftu_series_free(kc);

        dftu_dataframe_free(via_registry);
        dftu_dataframe_free(via_direct);
        dftu_lazyframe_free(direct);
        dftu_lazyframe_free(lf1);
        dftu_lazyframe_free(lf2);
        dftu_dataframe_free(df);
    }

    TEST_CASE(
        "dftu.lazy.group_by_dynamic matches dftu_lazyframe_group_by_dynamic") {
        dftu_dataframe* df = make_frame({0, 1, 2, 10, 11}, {1, 2, 3, 4, 5});
        REQUIRE(df != nullptr);

        check_kind_and_arity("dftu.lazy.group_by_dynamic", 1);
        dftu_lazyframe* lf1 = dftu_dataframe_lazy(df);
        dftu_lazyframe* lf2 = dftu_dataframe_lazy(df);
        dftu_group_agg aggs[1] = {{"sum", "v", "v_sum"}};
        OpArgs a;
        a.str(1, "k")
            .i64(2, 10)
            .i64(3, 10)
            .agglist(4, {aggs, 1})
            .i64(5, 0)
            .i32(6, 0);
        dftu_dataframe* via_registry =
            run_registry("dftu.lazy.group_by_dynamic", lf1, a);
        dftu_lazyframe* direct =
            dftu_lazyframe_group_by_dynamic(lf2, "k", 10, 10, aggs, 1, 0, 0);
        REQUIRE(direct != nullptr);
        dftu_dataframe* via_direct = dftu_lazyframe_collect(direct, 0);
        check_same(via_registry, via_direct);
        CHECK(dftu_dataframe_num_rows(via_registry) == 2);

        dftu_dataframe_free(via_registry);
        dftu_dataframe_free(via_direct);
        dftu_lazyframe_free(direct);
        dftu_lazyframe_free(lf1);
        dftu_lazyframe_free(lf2);
        dftu_dataframe_free(df);
    }

    TEST_CASE("collect/schema/explain/free stay out of the lazy op table") {
        CHECK(dftu_op_find("dftu.lazy.collect") == nullptr);
        CHECK(dftu_op_find("dftu.lazy.schema") == nullptr);
        CHECK(dftu_op_find("dftu.lazy.explain") == nullptr);
        CHECK(dftu_op_find("dftu.lazy.free") == nullptr);
    }
}
