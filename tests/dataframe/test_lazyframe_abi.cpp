// The dftu_lazyframe C ABI: wrap a materialized frame as a lazy plan, chain a
// couple of builder ops, and collect back to the eager equivalent. Handles are
// freed explicitly so asan flags any leak.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/expr.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

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

}  // namespace

TEST_SUITE("lazyframe_abi") {
    TEST_CASE("dftu_dataframe_lazy then filter collects the eager equivalent") {
        dftu_dataframe* df = make_frame({1, 1, 2, 3}, {5, 15, 20, 8});
        REQUIRE(df != nullptr);

        dftu_lazyframe* lf = dftu_dataframe_lazy(df);
        REQUIRE(lf != nullptr);

        // Keep rows where v (column 1) > 10.
        dftu_expr* col1 = dftu_expr_col(1);
        dftu_expr* pred = dftu_expr_cmp(DFTU_CMP_GT, col1, i64_scalar(10));
        dftu_expr_free(col1);  // a child is shared, the handle is the caller's
        REQUIRE(pred != nullptr);
        dftu_lazyframe* filtered = dftu_lazyframe_filter(lf, pred);
        REQUIRE(filtered != nullptr);

        dftu_dataframe* out = dftu_lazyframe_collect(filtered, 0);
        REQUIRE(out != nullptr);

        CHECK(dftu_dataframe_num_rows(out) == 2);
        dftu_series* vc = dftu_dataframe_column(out, "v");
        REQUIRE(vc != nullptr);
        const std::int64_t* vd = i64_of(vc);
        CHECK(vd[0] == 15);
        CHECK(vd[1] == 20);

        // collect does not consume the plan: re-running yields the same rows.
        dftu_dataframe* again = dftu_lazyframe_collect(filtered, 0);
        REQUIRE(again != nullptr);
        CHECK(dftu_dataframe_num_rows(again) == 2);

        dftu_series_free(vc);
        dftu_dataframe_free(again);
        dftu_dataframe_free(out);
        dftu_expr_free(pred);
        dftu_lazyframe_free(filtered);
        dftu_lazyframe_free(lf);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu_lazyframe_group_by sums per key") {
        dftu_dataframe* df = make_frame({1, 1, 2, 3}, {5, 15, 20, 8});
        REQUIRE(df != nullptr);

        dftu_lazyframe* lf = dftu_dataframe_lazy(df);
        REQUIRE(lf != nullptr);

        const char* keys[1] = {"k"};
        dftu_group_agg aggs[1] = {{"sum", "v", "v_sum"}};
        dftu_lazyframe* grouped = dftu_lazyframe_group_by(lf, keys, 1, aggs, 1);
        REQUIRE(grouped != nullptr);

        dftu_lazyframe* sorted = dftu_lazyframe_sort_by(grouped, "k", 0);
        REQUIRE(sorted != nullptr);

        dftu_dataframe* out = dftu_lazyframe_collect(sorted, 0);
        REQUIRE(out != nullptr);
        CHECK(dftu_dataframe_num_rows(out) == 3);

        dftu_series* kc = dftu_dataframe_column(out, "k");
        dftu_series* sc = dftu_dataframe_column(out, "v_sum");
        REQUIRE(kc != nullptr);
        REQUIRE(sc != nullptr);
        const std::int64_t* kd = i64_of(kc);
        const std::int64_t* sd = i64_of(sc);
        CHECK(kd[0] == 1);
        CHECK(sd[0] == 20);
        CHECK(kd[1] == 2);
        CHECK(sd[1] == 20);
        CHECK(kd[2] == 3);
        CHECK(sd[2] == 8);

        dftu_series_free(kc);
        dftu_series_free(sc);
        dftu_dataframe_free(out);
        dftu_lazyframe_free(sorted);
        dftu_lazyframe_free(grouped);
        dftu_lazyframe_free(lf);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu_lazyframe_rename then dftu_lazyframe_slice") {
        dftu_dataframe* df = make_frame({1, 2, 3, 4}, {10, 20, 30, 40});
        REQUIRE(df != nullptr);
        dftu_lazyframe* lf = dftu_dataframe_lazy(df);
        REQUIRE(lf != nullptr);

        const char* new_names[2] = {"key", "val"};
        dftu_lazyframe* renamed = dftu_lazyframe_rename(lf, new_names, 2);
        REQUIRE(renamed != nullptr);
        dftu_lazyframe* sliced = dftu_lazyframe_slice(renamed, 1, 2);
        REQUIRE(sliced != nullptr);

        dftu_dataframe* out = dftu_lazyframe_collect(sliced, 0);
        REQUIRE(out != nullptr);
        CHECK(dftu_dataframe_num_rows(out) == 2);
        CHECK(std::string(dftu_dataframe_column_name(out, 0)) == "key");
        dftu_series* vc = dftu_dataframe_column(out, "val");
        REQUIRE(vc != nullptr);
        const std::int64_t* vd = i64_of(vc);
        CHECK(vd[0] == 20);
        CHECK(vd[1] == 30);

        dftu_series_free(vc);
        dftu_dataframe_free(out);
        dftu_lazyframe_free(sliced);
        dftu_lazyframe_free(renamed);
        dftu_lazyframe_free(lf);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu_lazyframe_fill_null replaces nulls") {
        std::int64_t v[3] = {1, 0, 3};
        std::uint8_t validity = 0b101;  // row 1 is null
        dftu_series* vc =
            dftu_series_new_flat(DFTU_TYPE_INT64, v, 3, &validity);
        REQUIRE(vc != nullptr);
        const char* names[1] = {"v"};
        dftu_series* cols[1] = {vc};
        dftu_dataframe* df = dftu_dataframe_new(names, cols, 1);
        REQUIRE(df != nullptr);

        dftu_lazyframe* lf = dftu_dataframe_lazy(df);
        REQUIRE(lf != nullptr);
        dftu_lazyframe* filled = dftu_lazyframe_fill_null(lf, i64_scalar(99));
        REQUIRE(filled != nullptr);

        dftu_dataframe* out = dftu_lazyframe_collect(filled, 0);
        REQUIRE(out != nullptr);
        dftu_series* outc = dftu_dataframe_column(out, "v");
        REQUIRE(outc != nullptr);
        const std::int64_t* od = i64_of(outc);
        CHECK(od[0] == 1);
        CHECK(od[1] == 99);
        CHECK(od[2] == 3);

        dftu_series_free(outc);
        dftu_dataframe_free(out);
        dftu_lazyframe_free(filled);
        dftu_lazyframe_free(lf);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu_lazyframe_with_row_index and dftu_lazyframe_null_count") {
        dftu_dataframe* df = make_frame({1, 2}, {10, 20});
        REQUIRE(df != nullptr);
        dftu_lazyframe* lf = dftu_dataframe_lazy(df);
        REQUIRE(lf != nullptr);

        dftu_lazyframe* indexed = dftu_lazyframe_with_row_index(lf, "idx");
        REQUIRE(indexed != nullptr);
        dftu_dataframe* out = dftu_lazyframe_collect(indexed, 0);
        REQUIRE(out != nullptr);
        dftu_series* idxc = dftu_dataframe_column(out, "idx");
        REQUIRE(idxc != nullptr);
        const std::int64_t* idxd = i64_of(idxc);
        CHECK(idxd[0] == 0);
        CHECK(idxd[1] == 1);
        dftu_series_free(idxc);
        dftu_dataframe_free(out);
        dftu_lazyframe_free(indexed);

        dftu_lazyframe* nulls = dftu_lazyframe_null_count(lf);
        REQUIRE(nulls != nullptr);
        dftu_dataframe* nout = dftu_lazyframe_collect(nulls, 0);
        REQUIRE(nout != nullptr);
        CHECK(dftu_dataframe_num_rows(nout) == 1);
        dftu_series* kc = dftu_dataframe_column(nout, "k");
        REQUIRE(kc != nullptr);
        CHECK(i64_of(kc)[0] == 0);

        dftu_series_free(kc);
        dftu_dataframe_free(nout);
        dftu_lazyframe_free(nulls);
        dftu_lazyframe_free(lf);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu_lazyframe_explode expands a list column") {
        std::int32_t offsets[3] = {0, 2, 3};
        std::int64_t values[3] = {1, 2, 3};
        dftu_series* leaf =
            dftu_series_new_flat(DFTU_TYPE_INT64, values, 3, nullptr);
        REQUIRE(leaf != nullptr);
        dftu_series* list_col = dftu_series_new_list(offsets, 2, leaf);
        REQUIRE(list_col != nullptr);
        const char* names[1] = {"xs"};
        dftu_series* cols[1] = {list_col};
        dftu_dataframe* df = dftu_dataframe_new(names, cols, 1);
        REQUIRE(df != nullptr);

        dftu_lazyframe* lf = dftu_dataframe_lazy(df);
        REQUIRE(lf != nullptr);
        dftu_lazyframe* exploded = dftu_lazyframe_explode(lf, "xs");
        REQUIRE(exploded != nullptr);
        dftu_dataframe* out = dftu_lazyframe_collect(exploded, 0);
        REQUIRE(out != nullptr);
        CHECK(dftu_dataframe_num_rows(out) == 3);

        // An unknown column builds but fails at collect, not at the op call.
        dftu_lazyframe* bad = dftu_lazyframe_explode(lf, "missing");
        REQUIRE(bad != nullptr);
        CHECK(dftu_lazyframe_collect(bad, 0) == nullptr);
        dftu_lazyframe_free(bad);

        dftu_dataframe_free(out);
        dftu_lazyframe_free(exploded);
        dftu_lazyframe_free(lf);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu_lazyframe_unpivot and dftu_lazyframe_melt agree") {
        dftu_dataframe* df = make_frame({1, 2}, {10, 20});
        REQUIRE(df != nullptr);
        dftu_lazyframe* lf = dftu_dataframe_lazy(df);
        REQUIRE(lf != nullptr);

        const char* ids[1] = {"k"};
        const char* vals[1] = {"v"};
        dftu_lazyframe* up = dftu_lazyframe_unpivot(lf, ids, 1, vals, 1);
        REQUIRE(up != nullptr);
        dftu_dataframe* out1 = dftu_lazyframe_collect(up, 0);
        REQUIRE(out1 != nullptr);
        CHECK(dftu_dataframe_num_rows(out1) == 2);
        CHECK(dftu_dataframe_num_columns(out1) == 3);

        dftu_lazyframe* mp = dftu_lazyframe_melt(lf, ids, 1, vals, 1);
        REQUIRE(mp != nullptr);
        dftu_dataframe* out2 = dftu_lazyframe_collect(mp, 0);
        REQUIRE(out2 != nullptr);
        CHECK(dftu_dataframe_num_rows(out2) == dftu_dataframe_num_rows(out1));
        CHECK(dftu_dataframe_num_columns(out2) ==
              dftu_dataframe_num_columns(out1));

        dftu_dataframe_free(out2);
        dftu_dataframe_free(out1);
        dftu_lazyframe_free(mp);
        dftu_lazyframe_free(up);
        dftu_lazyframe_free(lf);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu_lazyframe_topk keeps the largest rows") {
        dftu_dataframe* df = make_frame({1, 2, 3, 4}, {40, 10, 30, 20});
        REQUIRE(df != nullptr);
        dftu_lazyframe* lf = dftu_dataframe_lazy(df);
        REQUIRE(lf != nullptr);

        dftu_lazyframe* top = dftu_lazyframe_topk(lf, "v", 2, 1);
        REQUIRE(top != nullptr);
        dftu_dataframe* out = dftu_lazyframe_collect(top, 0);
        REQUIRE(out != nullptr);
        CHECK(dftu_dataframe_num_rows(out) == 2);
        dftu_series* vc = dftu_dataframe_column(out, "v");
        REQUIRE(vc != nullptr);
        const std::int64_t* vd = i64_of(vc);
        CHECK(vd[0] == 40);
        CHECK(vd[1] == 30);

        dftu_series_free(vc);
        dftu_dataframe_free(out);
        dftu_lazyframe_free(top);
        dftu_lazyframe_free(lf);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu_lazyframe_sample bounds to n rows") {
        dftu_dataframe* df = make_frame({1, 2, 3, 4, 5}, {1, 2, 3, 4, 5});
        REQUIRE(df != nullptr);
        dftu_lazyframe* lf = dftu_dataframe_lazy(df);
        REQUIRE(lf != nullptr);

        dftu_lazyframe* sampled = dftu_lazyframe_sample(lf, 2, 42);
        REQUIRE(sampled != nullptr);
        dftu_dataframe* out = dftu_lazyframe_collect(sampled, 0);
        REQUIRE(out != nullptr);
        CHECK(dftu_dataframe_num_rows(out) == 2);

        dftu_dataframe_free(out);
        dftu_lazyframe_free(sampled);
        dftu_lazyframe_free(lf);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu_lazyframe_is_duplicated / is_unique flag rows") {
        dftu_dataframe* df = make_frame({1, 1, 2}, {5, 5, 9});
        REQUIRE(df != nullptr);
        dftu_lazyframe* lf = dftu_dataframe_lazy(df);
        REQUIRE(lf != nullptr);

        // The Bool result is an Arrow-layout bitmap: bit i of byte i/8.
        auto bit = [](const std::uint8_t* bytes, std::int64_t i) {
            return (bytes[i / 8] >> (i % 8)) & 1;
        };

        dftu_lazyframe* dup = dftu_lazyframe_is_duplicated(lf);
        REQUIRE(dup != nullptr);
        dftu_dataframe* dout = dftu_lazyframe_collect(dup, 0);
        REQUIRE(dout != nullptr);
        dftu_series* dc =
            dftu_dataframe_column(dout, dftu_dataframe_column_name(dout, 0));
        REQUIRE(dc != nullptr);
        const std::uint8_t* dd =
            static_cast<const std::uint8_t*>(dftu_series_data(dc));
        CHECK(bit(dd, 0) != 0);
        CHECK(bit(dd, 1) != 0);
        CHECK(bit(dd, 2) == 0);
        dftu_series_free(dc);
        dftu_dataframe_free(dout);
        dftu_lazyframe_free(dup);

        dftu_lazyframe* uniq = dftu_lazyframe_is_unique(lf);
        REQUIRE(uniq != nullptr);
        dftu_dataframe* uout = dftu_lazyframe_collect(uniq, 0);
        REQUIRE(uout != nullptr);
        dftu_series* uc =
            dftu_dataframe_column(uout, dftu_dataframe_column_name(uout, 0));
        REQUIRE(uc != nullptr);
        const std::uint8_t* ud =
            static_cast<const std::uint8_t*>(dftu_series_data(uc));
        CHECK(bit(ud, 0) == 0);
        CHECK(bit(ud, 1) == 0);
        CHECK(bit(ud, 2) != 0);
        dftu_series_free(uc);
        dftu_dataframe_free(uout);
        dftu_lazyframe_free(uniq);

        dftu_lazyframe_free(lf);
        dftu_dataframe_free(df);
    }

    TEST_CASE(
        "dftu_lazyframe_group_by_dynamic windows an ascending time column") {
        dftu_dataframe* df = make_frame({0, 1, 5, 6}, {10, 20, 30, 40});
        REQUIRE(df != nullptr);
        dftu_lazyframe* lf = dftu_dataframe_lazy(df);
        REQUIRE(lf != nullptr);

        dftu_group_agg aggs[1] = {{"sum", "v", "v_sum"}};
        dftu_lazyframe* windowed =
            dftu_lazyframe_group_by_dynamic(lf, "k", 5, 0, aggs, 1, 0, 0);
        REQUIRE(windowed != nullptr);
        dftu_dataframe* out = dftu_lazyframe_collect(windowed, 0);
        REQUIRE(out != nullptr);
        CHECK(dftu_dataframe_num_rows(out) == 2);  // [0,5) and [5,10)
        dftu_series* sc = dftu_dataframe_column(out, "v_sum");
        REQUIRE(sc != nullptr);
        const std::int64_t* sd = i64_of(sc);
        CHECK(sd[0] == 30);
        CHECK(sd[1] == 70);

        // `every <= 0` builds but fails at collect, not at the op call.
        dftu_lazyframe* bad_every =
            dftu_lazyframe_group_by_dynamic(lf, "k", 0, 0, aggs, 1, 0, 0);
        REQUIRE(bad_every != nullptr);
        CHECK(dftu_lazyframe_collect(bad_every, 0) == nullptr);
        dftu_lazyframe_free(bad_every);

        dftu_series_free(sc);
        dftu_dataframe_free(out);
        dftu_lazyframe_free(windowed);
        dftu_lazyframe_free(lf);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu_lazyframe_pivot and dftu_lazyframe_to_dummies reshape") {
        dftu_dataframe* df = make_frame({1, 2}, {10, 20});
        REQUIRE(df != nullptr);
        dftu_lazyframe* lf = dftu_dataframe_lazy(df);
        REQUIRE(lf != nullptr);

        dftu_lazyframe* pv = dftu_lazyframe_pivot(lf, "k", "k", "v", "first");
        REQUIRE(pv != nullptr);
        dftu_dataframe* pout = dftu_lazyframe_collect(pv, 0);
        REQUIRE(pout != nullptr);
        CHECK(dftu_dataframe_num_rows(pout) == 2);  // distinct k: 1, 2
        dftu_dataframe_free(pout);
        dftu_lazyframe_free(pv);

        // An unknown index column builds but fails at collect, not the op call.
        dftu_lazyframe* bad_pivot =
            dftu_lazyframe_pivot(lf, "missing", "k", "v", "first");
        REQUIRE(bad_pivot != nullptr);
        CHECK(dftu_lazyframe_collect(bad_pivot, 0) == nullptr);
        dftu_lazyframe_free(bad_pivot);

        dftu_lazyframe* dm = dftu_lazyframe_to_dummies(lf, "v");
        REQUIRE(dm != nullptr);
        dftu_dataframe* dout = dftu_lazyframe_collect(dm, 0);
        REQUIRE(dout != nullptr);
        // v has distinct 10,20 -> k, v_10, v_20
        CHECK(dftu_dataframe_num_columns(dout) == 3);
        dftu_dataframe_free(dout);
        dftu_lazyframe_free(dm);

        dftu_lazyframe_free(lf);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu_lazyframe_describe summarizes numeric columns") {
        dftu_dataframe* df = make_frame({1, 2, 3}, {10, 20, 30});
        REQUIRE(df != nullptr);
        dftu_lazyframe* lf = dftu_dataframe_lazy(df);
        REQUIRE(lf != nullptr);

        dftu_lazyframe* desc = dftu_lazyframe_describe(lf);
        REQUIRE(desc != nullptr);
        dftu_dataframe* out = dftu_lazyframe_collect(desc, 0);
        REQUIRE(out != nullptr);
        // One row per stat (count/null_count/mean/std/min/max).
        CHECK(dftu_dataframe_num_rows(out) == 6);
        CHECK(dftu_dataframe_num_columns(out) == 3);  // stat, k, v

        dftu_dataframe_free(out);
        dftu_lazyframe_free(desc);
        dftu_lazyframe_free(lf);
        dftu_dataframe_free(df);
    }

    TEST_CASE(
        "dftu_lazyframe_memory_budget and dftu_lazyframe_auto_spill "
        "still collect correctly") {
        dftu_dataframe* df = make_frame({3, 1, 2}, {30, 10, 20});
        REQUIRE(df != nullptr);
        dftu_lazyframe* lf = dftu_dataframe_lazy(df);
        REQUIRE(lf != nullptr);

        dftu_lazyframe* budgeted = dftu_lazyframe_memory_budget(lf, 1024);
        REQUIRE(budgeted != nullptr);
        dftu_lazyframe* sorted = dftu_lazyframe_sort_by(budgeted, "k", 0);
        REQUIRE(sorted != nullptr);
        dftu_dataframe* out = dftu_lazyframe_collect(sorted, 0);
        REQUIRE(out != nullptr);
        dftu_series* kc = dftu_dataframe_column(out, "k");
        REQUIRE(kc != nullptr);
        const std::int64_t* kd = i64_of(kc);
        CHECK(kd[0] == 1);
        CHECK(kd[1] == 2);
        CHECK(kd[2] == 3);
        dftu_series_free(kc);
        dftu_dataframe_free(out);
        dftu_lazyframe_free(sorted);
        dftu_lazyframe_free(budgeted);

        dftu_lazyframe* spill = dftu_lazyframe_auto_spill(lf);
        REQUIRE(spill != nullptr);
        dftu_dataframe* sout = dftu_lazyframe_collect(spill, 0);
        REQUIRE(sout != nullptr);
        CHECK(dftu_dataframe_num_rows(sout) == 3);

        dftu_dataframe_free(sout);
        dftu_lazyframe_free(spill);
        dftu_lazyframe_free(lf);
        dftu_dataframe_free(df);
    }

    TEST_CASE("dftu_lazyframe_drop_duplicates is dftu_lazyframe_unique") {
        dftu_dataframe* df = make_frame({1, 1, 2}, {5, 5, 9});
        REQUIRE(df != nullptr);
        dftu_lazyframe* lf = dftu_dataframe_lazy(df);
        REQUIRE(lf != nullptr);

        dftu_lazyframe* deduped = dftu_lazyframe_drop_duplicates(lf);
        REQUIRE(deduped != nullptr);
        dftu_dataframe* out = dftu_lazyframe_collect(deduped, 0);
        REQUIRE(out != nullptr);
        CHECK(dftu_dataframe_num_rows(out) == 2);

        dftu_dataframe_free(out);
        dftu_lazyframe_free(deduped);
        dftu_lazyframe_free(lf);
        dftu_dataframe_free(df);
    }

    TEST_CASE("a null handle is rejected, not crashed") {
        CHECK(dftu_dataframe_lazy(nullptr) == nullptr);
        CHECK(dftu_lazyframe_collect(nullptr, 0) == nullptr);
        CHECK(dftu_lazyframe_filter(nullptr, nullptr) == nullptr);
        CHECK(dftu_lazyframe_rename(nullptr, nullptr, 0) == nullptr);
        CHECK(dftu_lazyframe_slice(nullptr, 0, 1) == nullptr);
        CHECK(dftu_lazyframe_fill_null(nullptr, i64_scalar(0)) == nullptr);
        CHECK(dftu_lazyframe_with_row_index(nullptr, "x") == nullptr);
        CHECK(dftu_lazyframe_null_count(nullptr) == nullptr);
        CHECK(dftu_lazyframe_explode(nullptr, "x") == nullptr);
        CHECK(dftu_lazyframe_unpivot(nullptr, nullptr, 0, nullptr, 0) ==
              nullptr);
        CHECK(dftu_lazyframe_melt(nullptr, nullptr, 0, nullptr, 0) == nullptr);
        CHECK(dftu_lazyframe_topk(nullptr, "x", 1, 1) == nullptr);
        CHECK(dftu_lazyframe_sample(nullptr, 1, 0) == nullptr);
        CHECK(dftu_lazyframe_is_duplicated(nullptr) == nullptr);
        CHECK(dftu_lazyframe_is_unique(nullptr) == nullptr);
        CHECK(dftu_lazyframe_group_by_dynamic(nullptr, "x", 1, 0, nullptr, 0, 0,
                                              0) == nullptr);
        CHECK(dftu_lazyframe_pivot(nullptr, "x", "y", "z", nullptr) == nullptr);
        CHECK(dftu_lazyframe_to_dummies(nullptr, "x") == nullptr);
        CHECK(dftu_lazyframe_describe(nullptr) == nullptr);
        CHECK(dftu_lazyframe_memory_budget(nullptr, 1) == nullptr);
        CHECK(dftu_lazyframe_auto_spill(nullptr) == nullptr);
        CHECK(dftu_lazyframe_drop_duplicates(nullptr) == nullptr);
        dftu_lazyframe_free(nullptr);
    }
}
