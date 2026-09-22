#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/op.h>
#include <dftracer/utils/query/abi.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

using dftracer::utils::dataframe::Agg;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::find_op;
using dftracer::utils::dataframe::GroupAgg;
using dftracer::utils::dataframe::OpArgs;
using dftracer::utils::dataframe::OpKind;
using dftracer::utils::dataframe::Series;

namespace {

dftu_series* i64_col(const std::int64_t* v, std::int64_t n) {
    return dftu_series_new_flat(DFTU_TYPE_INT64, v, n, nullptr);
}

const std::int64_t* i64_of(const dftu_series* c) {
    return static_cast<const std::int64_t*>(dftu_series_data(c));
}

bool bit_of(const dftu_series* c, std::int64_t i) {
    const auto* d = static_cast<const std::uint8_t*>(dftu_series_data(c));
    return ((d[i >> 3] >> (i & 7)) & 1u) != 0;
}

dftu_scalar i64_scalar(std::int64_t v) {
    dftu_scalar s;
    s.kind = DFTU_SCALAR_TAG_I64;
    s.value.i = v;
    return s;
}

}  // namespace

TEST_SUITE("op_registry") {
    TEST_CASE("a built-in binary op is found and its signature decodes") {
        auto add = find_op("dftu.series.add");
        REQUIRE(add.has_value());
        CHECK(add->sig().kind() == OpKind::Series);
        CHECK(add->sig().arity() == 2);
        CHECK(add->sig().to_string() == "(series, series) -> series");

        std::int64_t a[3] = {1, 2, 3}, b[3] = {10, 20, 30};
        dftu_series* ca = i64_col(a, 3);
        dftu_series* cb = i64_col(b, 3);
        const dftu_series* in[2] = {ca, cb};
        dftu_series* out =
            dftu_op_run(dftu_op_find("dftu.series.add"), in, 2, nullptr);
        REQUIRE(out != nullptr);
        const std::int64_t* d = i64_of(out);
        CHECK(d[0] == 11);
        CHECK(d[2] == 33);
        dftu_series_free(out);
        dftu_series_free(ca);
        dftu_series_free(cb);
    }

    TEST_CASE(
        "scalar / enum / string / f64 operands ride args[] positionally") {
        std::int64_t v[3] = {1, 5, 2};
        dftu_series* c = i64_col(v, 3);
        const dftu_series* in[1] = {c};

        // add_scalar: SCALAR at operand slot 1.
        OpArgs a1;
        a1.scalar(1, i64_scalar(100));
        dftu_series* o1 =
            dftu_op_run(dftu_op_find("dftu.series.add_scalar"), in, 1, a1);
        REQUIRE(o1 != nullptr);
        CHECK(i64_of(o1)[0] == 101);
        dftu_series_free(o1);

        // compare: enum at slot 1, scalar at slot 2 -> v > 3.
        OpArgs a2;
        a2.i32(1, DFTU_CMP_GT).scalar(2, i64_scalar(3));
        dftu_series* o2 =
            dftu_op_run(dftu_op_find("dftu.series.compare"), in, 1, a2);
        REQUIRE(o2 != nullptr);
        CHECK(bit_of(o2, 1) == true);
        CHECK(bit_of(o2, 0) == false);
        dftu_series_free(o2);
        dftu_series_free(c);

        // str_contains: str at slot 1.
        std::int32_t offs[3] = {0, 6, 11};
        dftu_series* sc = dftu_series_new_string(DFTU_TYPE_STRING, offs,
                                                 "foobarhello", 2, nullptr);
        const dftu_series* sin[1] = {sc};
        OpArgs a3;
        a3.str(1, "oo");
        dftu_series* o3 =
            dftu_op_run(dftu_op_find("dftu.series.str_contains"), sin, 1, a3);
        REQUIRE(o3 != nullptr);
        CHECK(bit_of(o3, 0) == true);
        CHECK(bit_of(o3, 1) == false);
        dftu_series_free(o3);
        dftu_series_free(sc);
    }

    TEST_CASE("reducers run through dftu_op_run_aggregate") {
        std::int64_t v[3] = {1, 5, 2};
        dftu_series* c = i64_col(v, 3);
        int ok = 0;
        CHECK(dftu_op_run_aggregate(dftu_op_find("dftu.series.count"), c,
                                    nullptr, &ok)
                  .value.i == 3);
        CHECK(ok == 1);
        CHECK(dftu_op_run_aggregate(dftu_op_find("dftu.series.arg_max"), c,
                                    nullptr, &ok)
                  .value.i == 1);

        std::int64_t p[3] = {2, 3, 4};
        dftu_series* cp = i64_col(p, 3);
        CHECK(dftu_op_run_aggregate(dftu_op_find("dftu.series.product"), cp,
                                    nullptr, &ok)
                  .value.i == 24);
        OpArgs arg;
        arg.i32(1, DFTU_REDUCE_SUM);
        CHECK(dftu_op_run_aggregate(dftu_op_find("dftu.series.reduce"), cp, arg,
                                    &ok)
                  .value.i == 9);

        // dot: second series rides args[1].series -> 2*2 + 3*3 + 4*4 = 29.
        OpArgs darg;
        darg.series(1, cp);
        CHECK(dftu_op_run_aggregate(dftu_op_find("dftu.series.dot"), cp, darg,
                                    &ok)
                  .value.d == doctest::Approx(29.0));
        CHECK(ok == 1);
        dftu_series_free(c);
        dftu_series_free(cp);
    }

    TEST_CASE("the whole column-op surface is registered and listable") {
        for (const char* name :
             {"dftu.series.add", "dftu.series.compare", "dftu.series.cast",
              "dftu.series.prim", "dftu.series.logical",
              "dftu.series.str_contains", "dftu.series.str_replace",
              "dftu.series.str_slice", "dftu.series.count",
              "dftu.series.reduce", "dftu.series.mode"})
            CHECK(dftu_op_find(name) != nullptr);
        CHECK(dftu_op_find("no_such_op") == nullptr);

        using dftracer::utils::dataframe::op_at;
        using dftracer::utils::dataframe::op_count;

        uint32_t n = op_count();
        REQUIRE(n >= 120);  // ~100 column ops + ~23 frame ops
        bool saw_add = false, saw_frame = false;
        for (uint32_t i = 0; i < n; ++i) {
            auto op = op_at(i);
            REQUIRE(static_cast<bool>(op));
            if (op.name() == "dftu.series.add") saw_add = true;
            if (op.name() == "dftu.frame.head") saw_frame = true;
        }
        CHECK(saw_add);
        CHECK(saw_frame);
        CHECK(!static_cast<bool>(op_at(n)));
    }

    TEST_CASE("newly-added column ops and sig shapes run") {
        for (const char* name :
             {"dftu.series.abs", "dftu.series.sqrt", "dftu.series.cumsum",
              "dftu.series.fillna", "dftu.series.shift", "dftu.series.is_in",
              "dftu.series.nunique", "dftu.series.variance",
              "dftu.series.stddev", "dftu.series.quantile",
              "dftu.series.skewness", "dftu.series.clip",
              "dftu.series.is_between", "dftu.series.sort", "dftu.series.rank",
              "dftu.series.rolling", "dftu.series.ewm_mean"})
            CHECK(dftu_op_find(name) != nullptr);

        std::int64_t v[4] = {1, 2, 3, 4};
        dftu_series* c = i64_col(v, 4);
        const dftu_series* in1[1] = {c};
        int ok = 0;

        OpArgs varg;
        varg.i32(1, 1);  // sample
        dftu_scalar var = dftu_op_run_aggregate(
            dftu_op_find("dftu.series.variance"), c, varg, &ok);
        CHECK(ok == 1);
        CHECK(var.kind == DFTU_SCALAR_TAG_F64);
        CHECK(var.value.d == doctest::Approx(5.0 / 3.0));

        OpArgs qarg;
        qarg.f64(1, 0.5);
        dftu_scalar q = dftu_op_run_aggregate(
            dftu_op_find("dftu.series.quantile"), c, qarg, &ok);
        CHECK(q.kind == DFTU_SCALAR_TAG_F64);

        OpArgs carg;
        carg.scalar(1, i64_scalar(2)).scalar(2, i64_scalar(3));
        dftu_series* clipped =
            dftu_op_run(dftu_op_find("dftu.series.clip"), in1, 1, carg);
        REQUIRE(clipped != nullptr);
        CHECK(i64_of(clipped)[0] == 2);
        CHECK(i64_of(clipped)[3] == 3);
        dftu_series_free(clipped);

        dftu_series* cs =
            dftu_op_run(dftu_op_find("dftu.series.cumsum"), in1, 1, nullptr);
        REQUIRE(cs != nullptr);
        CHECK(i64_of(cs)[3] == 10);
        dftu_series_free(cs);
        dftu_series_free(c);
    }

    TEST_CASE("frame ops run via dftu_op_run_frame") {
        std::int64_t a[4] = {3, 1, 2, 1}, b[4] = {10, 20, 30, 40};
        const char* names[2] = {"a", "b"};
        dftu_series* cols[2] = {i64_col(a, 4), i64_col(b, 4)};  // moved into df
        dftu_dataframe* df = dftu_dataframe_new(names, cols, 2);
        REQUIRE(df != nullptr);
        const dftu_dataframe* fin[1] = {df};

        auto head = find_op("dftu.frame.head");
        REQUIRE(head.has_value());
        CHECK(head->sig().kind() == OpKind::Frame);
        OpArgs harg;
        harg.i64(1, 2);
        dftu_dataframe* h =
            dftu_op_run_frame(dftu_op_find("dftu.frame.head"), fin, 1, harg);
        REQUIRE(h != nullptr);
        CHECK(dftu_dataframe_num_rows(h) == 2);
        dftu_dataframe_free(h);

        const char* sel[1] = {"a"};
        OpArgs sarg;
        sarg.strlist(1, sel, 1);
        dftu_dataframe* s =
            dftu_op_run_frame(dftu_op_find("dftu.frame.select"), fin, 1, sarg);
        REQUIRE(s != nullptr);
        CHECK(dftu_dataframe_num_columns(s) == 1);
        dftu_dataframe_free(s);

        dftu_dataframe_free(df);

        // value_counts: series -> frame (no frame operand; series in args[0]).
        std::int64_t vc[4] = {1, 1, 2, 3};
        dftu_series* cvc = i64_col(vc, 4);
        OpArgs vcarg;
        vcarg.series(0, cvc);
        dftu_dataframe* vcf = dftu_op_run_frame(
            dftu_op_find("dftu.frame.value_counts"), nullptr, 0, vcarg);
        REQUIRE(vcf != nullptr);
        CHECK(dftu_dataframe_num_rows(vcf) == 3);  // 3 distinct values
        dftu_dataframe_free(vcf);
        dftu_series_free(cvc);
    }

    TEST_CASE("built-in ops live under dftu.series/dftu.frame, not bare") {
        CHECK(dftu_op_find("add") == nullptr);
        CHECK(dftu_op_find("head") == nullptr);
        CHECK(dftu_op_find("select") == nullptr);
        CHECK(dftu_op_find("dftu.series.add") != nullptr);
        CHECK(dftu_op_find("dftu.frame.head") != nullptr);
    }

    TEST_CASE("a user op registers under a module prefix and runs") {
        dftu_op_desc copy{"mymod.copy", DFTU_OP_SIG(SERIES, SERIES, NONE, NONE),
                          reinterpret_cast<const void*>(&dftu_series_share)};
        REQUIRE(dftu_op_register(&copy) == 0);
        std::int64_t v[2] = {7, 9};
        dftu_series* src = i64_col(v, 2);
        const dftu_series* in[1] = {src};
        dftu_series* out =
            dftu_op_run(dftu_op_find("mymod.copy"), in, 1, nullptr);
        REQUIRE(out != nullptr);
        CHECK(i64_of(out)[1] == 9);
        dftu_series_free(out);
        dftu_series_free(src);
    }

    TEST_CASE("registration rejects a clash and run rejects a mismatch") {
        CHECK(dftu_op_register(nullptr) != 0);
        dftu_op_desc dup{"dftu.series.add",
                         DFTU_OP_SIG(SERIES, SERIES, SERIES, NONE),
                         reinterpret_cast<const void*>(&dftu_series_add)};
        CHECK(dftu_op_register(&dup) != 0);

        // A name is a stable public key, so a second registration of one
        // already taken fails instead of shadowing it.
        dftu_op_desc first{"mymod.once",
                           DFTU_OP_SIG(SERIES, SERIES, NONE, NONE),
                           reinterpret_cast<const void*>(&dftu_series_share)};
        REQUIRE(dftu_op_register(&first) == 0);
        dftu_op_desc second{"mymod.once",
                            DFTU_OP_SIG(SERIES, SERIES, NONE, NONE),
                            reinterpret_cast<const void*>(&dftu_series_abs)};
        CHECK(dftu_op_register(&second) != 0);
        CHECK(dftu_op_find("mymod.once")->fn == first.fn);
        std::int64_t v[1] = {1};
        dftu_series* c = i64_col(v, 1);
        const dftu_series* in[1] = {c};
        CHECK(dftu_op_run(dftu_op_find("dftu.series.add"), in, 1, nullptr) ==
              nullptr);
        dftu_series_free(c);
    }

    TEST_CASE(
        "newly-registered slice/materialize/take/sample ops run and "
        "match the direct C ABI call") {
        std::int64_t v[5] = {10, 20, 30, 40, 50};
        dftu_series* c = i64_col(v, 5);
        const dftu_series* in1[1] = {c};

        // dftu.series.slice
        OpArgs sliceArg;
        sliceArg.i64(1, 1).i64(2, 3);
        dftu_series* sliced =
            dftu_op_run(dftu_op_find("dftu.series.slice"), in1, 1, sliceArg);
        dftu_series* slicedDirect = dftu_series_slice(c, 1, 3);
        REQUIRE(sliced != nullptr);
        REQUIRE(slicedDirect != nullptr);
        REQUIRE(dftu_series_length(sliced) == dftu_series_length(slicedDirect));
        for (std::int64_t i = 0; i < dftu_series_length(sliced); ++i)
            CHECK(i64_of(sliced)[i] == i64_of(slicedDirect)[i]);
        dftu_series_free(sliced);
        dftu_series_free(slicedDirect);

        // dftu.series.materialize
        dftu_series* mat = dftu_op_run(dftu_op_find("dftu.series.materialize"),
                                       in1, 1, nullptr);
        dftu_series* matDirect = dftu_series_materialize(c);
        REQUIRE(mat != nullptr);
        REQUIRE(matDirect != nullptr);
        REQUIRE(dftu_series_length(mat) == dftu_series_length(matDirect));
        for (std::int64_t i = 0; i < dftu_series_length(mat); ++i)
            CHECK(i64_of(mat)[i] == i64_of(matDirect)[i]);
        dftu_series_free(mat);
        dftu_series_free(matDirect);

        // dftu.series.take
        std::int64_t idx[3] = {3, 0, 4};
        OpArgs takeArg;
        takeArg.i64list(1, idx);
        dftu_series* taken =
            dftu_op_run(dftu_op_find("dftu.series.take"), in1, 1, takeArg);
        dftu_series* takenDirect = dftu_series_take(c, idx, 3);
        REQUIRE(taken != nullptr);
        REQUIRE(takenDirect != nullptr);
        REQUIRE(dftu_series_length(taken) == dftu_series_length(takenDirect));
        for (std::int64_t i = 0; i < dftu_series_length(taken); ++i)
            CHECK(i64_of(taken)[i] == i64_of(takenDirect)[i]);
        dftu_series_free(taken);
        dftu_series_free(takenDirect);

        // dftu.series.sample: deterministic (mix64 hash), so a fixed seed
        // matches the direct call bit-for-bit.
        OpArgs sampleArg;
        sampleArg.i64(1, 2).u64(2, 42);
        dftu_series* sampled =
            dftu_op_run(dftu_op_find("dftu.series.sample"), in1, 1, sampleArg);
        dftu_series* sampledDirect = dftu_series_sample(c, 2, 42);
        REQUIRE(sampled != nullptr);
        REQUIRE(sampledDirect != nullptr);
        REQUIRE(dftu_series_length(sampled) ==
                dftu_series_length(sampledDirect));
        for (std::int64_t i = 0; i < dftu_series_length(sampled); ++i)
            CHECK(i64_of(sampled)[i] == i64_of(sampledDirect)[i]);
        dftu_series_free(sampled);
        dftu_series_free(sampledDirect);

        dftu_series_free(c);
    }

    TEST_CASE(
        "newly-registered frame take/sample/group_by_dynamic ops run "
        "and match the direct C ABI call") {
        std::int64_t a[5] = {1, 2, 3, 4, 5};
        std::int64_t t[5] = {0, 1, 2, 10, 11};
        const char* names[2] = {"a", "t"};
        dftu_series* cols[2] = {i64_col(a, 5), i64_col(t, 5)};  // moved into df
        dftu_dataframe* df = dftu_dataframe_new(names, cols, 2);
        REQUIRE(df != nullptr);
        const dftu_dataframe* fin[1] = {df};

        // dftu.frame.take
        std::int64_t idx[2] = {4, 1};
        OpArgs takeArg;
        takeArg.i64list(1, idx);
        dftu_dataframe* taken =
            dftu_op_run_frame(dftu_op_find("dftu.frame.take"), fin, 1, takeArg);
        dftu_dataframe* takenDirect = dftu_dataframe_take(df, idx, 2);
        REQUIRE(taken != nullptr);
        REQUIRE(takenDirect != nullptr);
        CHECK(dftu_dataframe_num_rows(taken) ==
              dftu_dataframe_num_rows(takenDirect));
        dftu_series* takenCol = dftu_dataframe_column(taken, "a");
        dftu_series* takenDirectCol = dftu_dataframe_column(takenDirect, "a");
        for (std::int64_t i = 0; i < dftu_series_length(takenCol); ++i)
            CHECK(i64_of(takenCol)[i] == i64_of(takenDirectCol)[i]);
        dftu_series_free(takenCol);
        dftu_series_free(takenDirectCol);
        dftu_dataframe_free(taken);
        dftu_dataframe_free(takenDirect);

        // dftu.frame.sample: deterministic, same seed matches bit-for-bit.
        OpArgs sampleArg;
        sampleArg.i64(1, 2).u64(2, 7);
        dftu_dataframe* sampled = dftu_op_run_frame(
            dftu_op_find("dftu.frame.sample"), fin, 1, sampleArg);
        dftu_dataframe* sampledDirect = dftu_dataframe_sample(df, 2, 7);
        REQUIRE(sampled != nullptr);
        REQUIRE(sampledDirect != nullptr);
        CHECK(dftu_dataframe_num_rows(sampled) ==
              dftu_dataframe_num_rows(sampledDirect));
        dftu_dataframe_free(sampled);
        dftu_dataframe_free(sampledDirect);

        // dftu.frame.group_by_dynamic
        dftu_group_agg aggs[1] = {{"sum", "a", "asum"}};
        OpArgs gbdArg;
        gbdArg.str(1, "t").i64(2, 5).i64(3, -1).agglist(4, aggs);
        dftu_dataframe* windowed = dftu_op_run_frame(
            dftu_op_find("dftu.frame.group_by_dynamic"), fin, 1, gbdArg);
        dftu_dataframe* windowedDirect =
            dftu_dataframe_group_by_dynamic(df, "t", 5, -1, aggs, 1);
        REQUIRE(windowed != nullptr);
        REQUIRE(windowedDirect != nullptr);
        CHECK(dftu_dataframe_num_rows(windowed) ==
              dftu_dataframe_num_rows(windowedDirect));
        CHECK(dftu_dataframe_num_columns(windowed) ==
              dftu_dataframe_num_columns(windowedDirect));
        dftu_series* wsum = dftu_dataframe_column(windowed, "asum");
        dftu_series* wsumDirect = dftu_dataframe_column(windowedDirect, "asum");
        REQUIRE(wsum != nullptr);
        REQUIRE(wsumDirect != nullptr);
        for (std::int64_t i = 0; i < dftu_series_length(wsum); ++i)
            CHECK(i64_of(wsum)[i] == i64_of(wsumDirect)[i]);
        dftu_series_free(wsum);
        dftu_series_free(wsumDirect);
        dftu_dataframe_free(windowed);
        dftu_dataframe_free(windowedDirect);

        dftu_dataframe_free(df);
    }

    TEST_CASE(
        "dftu.frame.group_by runs via the registry and matches the direct "
        "C ABI call and DataFrame::group_by") {
        std::int64_t k[5] = {1, 2, 1, 2, 1};
        std::int64_t v[5] = {10, 20, 30, 40, 50};
        const char* names[2] = {"k", "v"};
        dftu_series* cols[2] = {i64_col(k, 5), i64_col(v, 5)};  // moved into df
        dftu_dataframe* df = dftu_dataframe_new(names, cols, 2);
        REQUIRE(df != nullptr);
        const dftu_dataframe* fin[1] = {df};

        const char* keys[1] = {"k"};
        dftu_group_agg aggs[1] = {{"sum", "v", "total"}};

        OpArgs gbArg;
        gbArg.strlist(1, keys, 1).agglist(2, aggs);
        dftu_dataframe* viaRegistry = dftu_op_run_frame(
            dftu_op_find("dftu.frame.group_by"), fin, 1, gbArg);
        dftu_dataframe* viaDirect =
            dftu_dataframe_group_by(df, keys, 1, aggs, 1);
        REQUIRE(viaRegistry != nullptr);
        REQUIRE(viaDirect != nullptr);
        REQUIRE(dftu_dataframe_num_rows(viaRegistry) ==
                dftu_dataframe_num_rows(viaDirect));

        dftu_series* kCol = dftu_dataframe_column(df, "k");
        dftu_series* vCol = dftu_dataframe_column(df, "v");
        DataFrame cppDf;
        cppDf.names = {"k", "v"};
        cppDf.columns.emplace_back(kCol);
        cppDf.columns.emplace_back(vCol);
        DataFrame viaCpp = cppDf.group_by(std::vector<std::string>{"k"},
                                          {GroupAgg{Agg::Sum, "v", "total"}});
        REQUIRE(static_cast<std::int64_t>(viaCpp.num_rows()) ==
                dftu_dataframe_num_rows(viaRegistry));

        dftu_series* rk = dftu_dataframe_column(viaRegistry, "k");
        dftu_series* rt = dftu_dataframe_column(viaRegistry, "total");
        dftu_series* dk = dftu_dataframe_column(viaDirect, "k");
        dftu_series* dt = dftu_dataframe_column(viaDirect, "total");
        Series ck = viaCpp.column("k");
        Series ct = viaCpp.column("total");
        for (std::int64_t i = 0; i < dftu_series_length(rk); ++i) {
            CHECK(i64_of(rk)[i] == i64_of(dk)[i]);
            CHECK(i64_of(rt)[i] == i64_of(dt)[i]);
            CHECK(i64_of(rk)[i] == i64_of(ck.handle())[i]);
            CHECK(i64_of(rt)[i] == i64_of(ct.handle())[i]);
        }
        dftu_series_free(rk);
        dftu_series_free(rt);
        dftu_series_free(dk);
        dftu_series_free(dt);
        dftu_dataframe_free(viaRegistry);
        dftu_dataframe_free(viaDirect);
        dftu_dataframe_free(df);
    }

    TEST_CASE(
        "table -> series ops (is_duplicated/is_unique/mask) run via "
        "dftu_op_run with the frame riding args[0]") {
        std::int64_t a[4] = {1, 2, 1, 3};
        const char* names[1] = {"a"};
        dftu_series* cols[1] = {i64_col(a, 4)};  // moved into df
        dftu_dataframe* df = dftu_dataframe_new(names, cols, 1);
        REQUIRE(df != nullptr);

        auto is_dup = find_op("dftu.frame.is_duplicated");
        REQUIRE(is_dup.has_value());
        CHECK(is_dup->sig().kind() == OpKind::Series);
        CHECK(is_dup->sig().arity() == 0);
        OpArgs dupArg;
        dupArg.frame(0, df);
        dftu_series* dup = dftu_op_run(dftu_op_find("dftu.frame.is_duplicated"),
                                       nullptr, 0, dupArg);
        dftu_series* dupDirect = dftu_dataframe_is_duplicated(df);
        REQUIRE(dup != nullptr);
        REQUIRE(dupDirect != nullptr);
        for (std::int64_t i = 0; i < dftu_series_length(dup); ++i)
            CHECK(bit_of(dup, i) == bit_of(dupDirect, i));
        dftu_series_free(dup);
        dftu_series_free(dupDirect);

        auto is_uniq = find_op("dftu.frame.is_unique");
        REQUIRE(is_uniq.has_value());
        CHECK(is_uniq->sig().kind() == OpKind::Series);
        CHECK(is_uniq->sig().arity() == 0);
        OpArgs uniqArg;
        uniqArg.frame(0, df);
        dftu_series* uniq = dftu_op_run(dftu_op_find("dftu.frame.is_unique"),
                                        nullptr, 0, uniqArg);
        dftu_series* uniqDirect = dftu_dataframe_is_unique(df);
        REQUIRE(uniq != nullptr);
        REQUIRE(uniqDirect != nullptr);
        for (std::int64_t i = 0; i < dftu_series_length(uniq); ++i)
            CHECK(bit_of(uniq, i) == bit_of(uniqDirect, i));
        dftu_series_free(uniq);
        dftu_series_free(uniqDirect);

        auto mask = find_op("dftu.frame.mask");
        REQUIRE(mask.has_value());
        CHECK(mask->sig().kind() == OpKind::Series);
        CHECK(mask->sig().arity() == 0);
        dftu_query* q = dftu_query_parse("a > 1");
        REQUIRE(q != nullptr);
        OpArgs maskArg;
        maskArg.frame(0, df).query(1, q);
        dftu_series* m =
            dftu_op_run(dftu_op_find("dftu.frame.mask"), nullptr, 0, maskArg);
        dftu_series* mDirect = dftu_dataframe_mask_frame(df, q);
        REQUIRE(m != nullptr);
        REQUIRE(mDirect != nullptr);
        for (std::int64_t i = 0; i < dftu_series_length(m); ++i)
            CHECK(bit_of(m, i) == bit_of(mDirect, i));
        dftu_series_free(m);
        dftu_series_free(mDirect);
        dftu_query_free(q);

        dftu_dataframe_free(df);
    }

    TEST_CASE("DFTU_OP_SIG8 round-trips all 8 tokens of a widest signature") {
        dftu_op_sig sig =
            DFTU_OP_SIG8(LAZY, LAZY, STR, I64, I64, AGGLIST, I64, I32);
        CHECK(DFTU_OP_SIG_RET(sig) == DFTU_TOK_LAZY);
        CHECK(DFTU_OP_SIG_ARG(sig, 0) == DFTU_TOK_LAZY);
        CHECK(DFTU_OP_SIG_ARG(sig, 1) == DFTU_TOK_STR);
        CHECK(DFTU_OP_SIG_ARG(sig, 2) == DFTU_TOK_I64);
        CHECK(DFTU_OP_SIG_ARG(sig, 3) == DFTU_TOK_I64);
        CHECK(DFTU_OP_SIG_ARG(sig, 4) == DFTU_TOK_AGGLIST);
        CHECK(DFTU_OP_SIG_ARG(sig, 5) == DFTU_TOK_I64);
        CHECK(DFTU_OP_SIG_ARG(sig, 6) == DFTU_TOK_I32);
    }
}
