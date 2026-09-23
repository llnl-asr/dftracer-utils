// The string Expr nodes: each against hand-computed rows, infer_type against
// eval, the C ABI builders, and the typed refusals.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using dftracer::utils::dataframe::DataType;
using dftracer::utils::dataframe::eval;
using dftracer::utils::dataframe::Expr;
using dftracer::utils::dataframe::expr_as_col_is_in;
using dftracer::utils::dataframe::expr_as_col_str_pred;
using dftracer::utils::dataframe::expr_col;
using dftracer::utils::dataframe::expr_is_in;
using dftracer::utils::dataframe::expr_is_null;
using dftracer::utils::dataframe::expr_lower;
using dftracer::utils::dataframe::expr_not;
using dftracer::utils::dataframe::expr_remap_cols;
using dftracer::utils::dataframe::expr_str_find;
using dftracer::utils::dataframe::expr_str_len;
using dftracer::utils::dataframe::expr_str_map;
using dftracer::utils::dataframe::expr_str_pred;
using dftracer::utils::dataframe::expr_str_replace;
using dftracer::utils::dataframe::expr_str_slice;
using dftracer::utils::dataframe::infer_type;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::StrMapOp;
using dftracer::utils::dataframe::StrPredOp;
using dftracer::utils::dataframe::TypeId;

namespace {

Series names() {
    return Series::strings({"read", "pread64", "Write", "fsync", "readv"});
}

std::vector<bool> bools(const Series& mask) {
    REQUIRE(mask.valid());
    REQUIRE(mask.type() == TypeId::Bool);
    std::vector<bool> out;
    const std::uint8_t* b = mask.data<std::uint8_t>();
    for (std::int64_t i = 0; i < mask.length(); ++i)
        out.push_back(((b[i >> 3] >> (i & 7)) & 1) != 0);
    return out;
}

std::vector<std::string> strs(const Series& s) {
    REQUIRE(s.valid());
    REQUIRE(s.type() == TypeId::String);
    std::vector<std::string> out;
    for (std::int64_t i = 0; i < s.length(); ++i)
        out.emplace_back(s.string_at(i));
    return out;
}

std::vector<std::int64_t> i64s(const Series& s) {
    REQUIRE(s.valid());
    REQUIRE(s.type() == TypeId::Int64);
    return std::vector<std::int64_t>(s.data<std::int64_t>(),
                                     s.data<std::int64_t>() + s.length());
}

// Every string node must type the same way it evaluates.
void check_inferred(const Expr& e, const Series& in) {
    std::vector<const Series*> ins{&in};
    Series out = eval(e, ins);
    REQUIRE(out.valid());
    DataType dt = infer_type(e, {in.data_type()});
    CHECK(dt.id == out.type());
}

}  // namespace

TEST_SUITE("expr string ops") {
    TEST_CASE("predicates against a literal") {
        Series s = names();
        std::vector<const Series*> in{&s};
        using B = std::vector<bool>;
        CHECK(
            bools(eval(expr_str_pred(StrPredOp::Contains, expr_col(0), "read"),
                       in)) == B{true, true, false, false, true});
        CHECK(
            bools(eval(expr_str_pred(StrPredOp::StartsWith, expr_col(0), "re"),
                       in)) == B{true, false, false, false, true});
        CHECK(bools(eval(expr_str_pred(StrPredOp::EndsWith, expr_col(0), "64"),
                         in)) == B{false, true, false, false, false});
        CHECK(bools(eval(expr_str_pred(StrPredOp::Like, expr_col(0), "%ead%"),
                         in)) == B{true, true, false, false, true});
        CHECK(bools(eval(expr_str_pred(StrPredOp::Like, expr_col(0), "r_ad_"),
                         in)) == B{false, false, false, false, true});
        CHECK(
            bools(eval(expr_str_pred(StrPredOp::Matches, expr_col(0), "[a-z]+"),
                       in)) == B{true, false, false, true, true});
        CHECK(bools(eval(expr_str_pred(StrPredOp::Search, expr_col(0), "[0-9]"),
                         in)) == B{false, true, false, false, false});
        // Matches is whole-string; Search is anywhere. Same pattern, two rows
        // differ.
        CHECK(bools(eval(expr_str_pred(StrPredOp::Matches, expr_col(0), "ead"),
                         in)) == B{false, false, false, false, false});
        CHECK(bools(eval(expr_str_pred(StrPredOp::Search, expr_col(0), "ead"),
                         in)) == B{true, true, false, false, true});
        // Case matters for Contains; lower() first for a case-fold.
        CHECK(
            bools(eval(expr_str_pred(StrPredOp::Contains, expr_col(0), "write"),
                       in)) == B{false, false, false, false, false});
        CHECK(bools(eval(expr_str_pred(StrPredOp::Contains,
                                       expr_lower(expr_col(0)), "write"),
                         in)) == B{false, false, true, false, false});
        for (StrPredOp op :
             {StrPredOp::Contains, StrPredOp::StartsWith, StrPredOp::EndsWith,
              StrPredOp::Like, StrPredOp::Matches, StrPredOp::Search})
            check_inferred(expr_str_pred(op, expr_col(0), "re"), s);
    }

    TEST_CASE("maps, length, find, replace, slice") {
        Series s = Series::strings({"  Read ", "pread64", "x"});
        std::vector<const Series*> in{&s};
        using S = std::vector<std::string>;
        using I = std::vector<std::int64_t>;
        CHECK(strs(eval(expr_str_map(StrMapOp::Upper, expr_col(0)), in)) ==
              S{"  READ ", "PREAD64", "X"});
        CHECK(strs(eval(expr_str_map(StrMapOp::Lower, expr_col(0)), in)) ==
              S{"  read ", "pread64", "x"});
        CHECK(strs(eval(expr_str_map(StrMapOp::Strip, expr_col(0)), in)) ==
              S{"Read", "pread64", "x"});
        CHECK(strs(eval(expr_str_map(StrMapOp::Lstrip, expr_col(0)), in)) ==
              S{"Read ", "pread64", "x"});
        CHECK(strs(eval(expr_str_map(StrMapOp::Rstrip, expr_col(0)), in)) ==
              S{"  Read", "pread64", "x"});
        CHECK(i64s(eval(expr_str_len(expr_col(0), false), in)) == I{7, 7, 1});
        CHECK(i64s(eval(expr_str_find(expr_col(0), "ead"), in)) == I{3, 2, -1});
        CHECK(strs(eval(expr_str_replace(expr_col(0), "ea", "EA", false),
                        in)) == S{"  REAd ", "prEAd64", "x"});
        Series rr = Series::strings({"aXaXa"});
        std::vector<const Series*> rin{&rr};
        CHECK(strs(eval(expr_str_replace(expr_col(0), "a", "b", false), rin)) ==
              S{"bXaXa"});
        CHECK(strs(eval(expr_str_replace(expr_col(0), "a", "b", true), rin)) ==
              S{"bXbXb"});
        CHECK(strs(eval(expr_str_slice(expr_col(0), 2, 3), in)) ==
              S{"Rea", "ead", ""});
        Series utf = Series::strings({"héllo"});
        std::vector<const Series*> uin{&utf};
        CHECK(i64s(eval(expr_str_len(expr_col(0), false), uin)) == I{6});
        CHECK(i64s(eval(expr_str_len(expr_col(0), true), uin)) == I{5});

        check_inferred(expr_str_map(StrMapOp::Upper, expr_col(0)), s);
        check_inferred(expr_str_len(expr_col(0), true), s);
        check_inferred(expr_str_find(expr_col(0), "e"), s);
        check_inferred(expr_str_replace(expr_col(0), "e", "E", true), s);
        check_inferred(expr_str_slice(expr_col(0), 0, 1), s);
    }

    TEST_CASE("is_in over strings and ints, and its negation") {
        Series s = names();
        std::vector<const Series*> in{&s};
        Expr e = expr_is_in(expr_col(0), Series::strings({"read", "fsync"}));
        CHECK(bools(eval(e, in)) ==
              std::vector<bool>{true, false, false, true, false});
        CHECK(bools(eval(expr_not(e), in)) ==
              std::vector<bool>{false, true, true, false, true});
        check_inferred(e, s);

        std::vector<std::int64_t> v{1, 2, 3, 4};
        Series ints = Series::flat_i64(v.data(), 4);
        std::vector<std::int64_t> set{2, 4};
        std::vector<const Series*> iin{&ints};
        CHECK(
            bools(eval(expr_is_in(expr_col(0), Series::flat_i64(set.data(), 2)),
                       iin)) == std::vector<bool>{false, true, false, true});
    }

    TEST_CASE("string ops CSE by text, not by pointer") {
        Series s = names();
        std::vector<const Series*> in{&s};
        std::string a = "read";
        std::string b = "read";
        std::string c = "rea";
        Expr same = expr_str_pred(StrPredOp::Contains, expr_col(0), a) &
                    expr_str_pred(StrPredOp::Contains, expr_col(0), b);
        Expr diff = expr_str_pred(StrPredOp::Contains, expr_col(0), a) &
                    expr_str_pred(StrPredOp::StartsWith, expr_col(0), c);
        CHECK(bools(eval(same, in)) ==
              std::vector<bool>{true, true, false, false, true});
        CHECK(bools(eval(diff, in)) ==
              std::vector<bool>{true, false, false, false, true});
    }

    TEST_CASE("refusals name the type") {
        std::vector<std::int64_t> v{1, 2};
        Series ints = Series::flat_i64(v.data(), 2);
        std::vector<const Series*> in{&ints};
        CHECK_THROWS_AS(
            (void)eval(expr_str_pred(StrPredOp::Contains, expr_col(0), "x"),
                       in),
            std::invalid_argument);
        CHECK_THROWS_AS(
            (void)eval(expr_str_map(StrMapOp::Upper, expr_col(0)), in),
            std::invalid_argument);
        CHECK_THROWS_AS(
            (void)eval(expr_is_in(expr_col(0), Series::strings({"a"})), in),
            std::invalid_argument);
        Series s = names();
        std::vector<const Series*> sin{&s};
        std::vector<std::int64_t> set{1};
        CHECK_THROWS_AS(
            (void)eval(expr_is_in(expr_col(0), Series::flat_i64(set.data(), 1)),
                       sin),
            std::invalid_argument);
        CHECK(infer_type(expr_str_len(expr_col(0), false),
                         {dftracer::utils::dataframe::scalar(TypeId::Unknown)})
                  .id == TypeId::Unknown);
    }

    TEST_CASE("planner accessors and column remap keep the pattern and set") {
        Expr p = expr_str_pred(StrPredOp::Like, expr_col(3), "a%");
        std::int32_t col = -1;
        StrPredOp op{};
        std::string_view pattern;
        REQUIRE(expr_as_col_str_pred(p, &col, &op, &pattern));
        CHECK(col == 3);
        CHECK(op == StrPredOp::Like);
        CHECK(pattern == "a%");
        Expr moved = expr_remap_cols(p, {0, 0, 0, 1});
        REQUIRE(expr_as_col_str_pred(moved, &col, &op, &pattern));
        CHECK(col == 1);
        CHECK(pattern == "a%");

        Expr e = expr_is_in(expr_col(0), Series::strings({"a", "b"}));
        Series values;
        REQUIRE(expr_as_col_is_in(expr_remap_cols(e, {2}), &col, &values));
        CHECK(col == 2);
        CHECK(values.length() == 2);
        CHECK_FALSE(expr_as_col_str_pred(e, &col, &op, &pattern));
        CHECK_FALSE(expr_as_col_is_in(p, &col, &values));
    }

    TEST_CASE("select picks per row and promotes its arms") {
        using dftracer::utils::dataframe::CmpOp;
        using dftracer::utils::dataframe::expr_cmp;
        using dftracer::utils::dataframe::expr_lit;
        using dftracer::utils::dataframe::expr_select;
        std::vector<std::int64_t> xv{1, 5, 3, 9};
        std::vector<std::uint8_t> valid{0x0D};  // row 1 null
        Series x = Series::flat_i64(xv.data(), 4, valid.data());
        std::vector<const Series*> in{&x};
        Expr big =
            expr_cmp(CmpOp::Gt, expr_col(0),
                     dftracer::utils::dataframe::to_scalar(std::int64_t{2}));

        // column : literal
        Series r =
            eval(expr_select(big, expr_col(0), expr_lit(std::int64_t{0})), in);
        REQUIRE(r.valid());
        CHECK(r.type() == TypeId::Int64);
        // Row 1 is null in x, so the condition is null there and the else arm
        // is picked: a present 0, not a null.
        CHECK(i64s(r) == std::vector<std::int64_t>{0, 0, 3, 9});
        CHECK_FALSE(r.is_null(1));

        // literal : literal, float promotes
        Series f = eval(
            expr_select(big, expr_lit(1.5), expr_lit(std::int64_t{0})), in);
        CHECK(f.type() == TypeId::Float64);
        CHECK(f.data<double>()[3] == 1.5);
        CHECK(f.data<double>()[1] == 0.0);
        CHECK(f.data<double>()[0] == 0.0);

        // column : column of a different int type meets at Int64. A null
        // condition (row 1: null > 2) takes the else arm, as SQL CASE does,
        // and a picked null arm (row 0 of y) stays null.
        std::vector<std::int32_t> yv{-1, -2, -3, -4};
        std::vector<std::uint8_t> yvalid{0x0E};  // row 0 null
        Series y = Series::flat(TypeId::Int32, yv.data(), 4, yvalid.data());
        std::vector<const Series*> in2{&x, &y};
        Series m = eval(expr_select(big, expr_col(0), expr_col(1)), in2);
        CHECK(m.type() == TypeId::Int64);
        CHECK(i64s(m)[1] == -2);
        CHECK(i64s(m)[2] == 3);
        CHECK(i64s(m)[3] == 9);
        CHECK(m.is_null(0));
        CHECK_FALSE(m.is_null(1));
        Series flipped =
            eval(expr_select(expr_not(big), expr_col(0), expr_col(1)), in2);
        CHECK(i64s(flipped)[0] == 1);
        CHECK(i64s(flipped)[1] == -2);  // NOT null is still null: else arm
        CHECK_FALSE(flipped.is_null(1));
        check_inferred(expr_select(big, expr_col(0), expr_lit(std::int64_t{0})),
                       x);

        // strings
        Series s = names();
        std::vector<const Series*> sin{&s};
        Series alt = Series::strings({"a", "b", "c", "d", "e"});
        std::vector<const Series*> sin2{&s, &alt};
        Series ws = eval(
            expr_select(expr_str_pred(StrPredOp::StartsWith, expr_col(0), "re"),
                        expr_col(0), expr_col(1)),
            sin2);
        CHECK(strs(ws) ==
              std::vector<std::string>{"read", "b", "c", "d", "readv"});
        CHECK_THROWS_AS((void)eval(expr_select(expr_col(0), expr_col(0),
                                               expr_lit(std::int64_t{0})),
                                   in),
                        std::invalid_argument);  // non-Bool condition

        dftu_expr* c = dftu_expr_col(0);
        dftu_expr* cond = dftu_expr_cmp(
            DFTU_CMP_GT, c,
            dftracer::utils::dataframe::to_scalar(std::int64_t{2}));
        dftu_expr* zero = dftu_expr_lit_i64(0);
        dftu_expr* sel = dftu_expr_select(cond, c, zero);
        REQUIRE(sel);
        const dftu_series* raw[1] = {x.handle()};
        Series via_c{dftu_expr_eval(sel, raw, 1)};
        CHECK(i64s(via_c) == std::vector<std::int64_t>{0, 0, 3, 9});
        CHECK(dftu_expr_select(nullptr, c, zero) == nullptr);
        for (dftu_expr* e : {sel, zero, cond, c}) dftu_expr_free(e);
    }

    TEST_CASE("C ABI builders") {
        Series s = names();
        dftu_expr* c = dftu_expr_col(0);
        dftu_expr* pred =
            dftu_expr_str_pred(DFTU_STR_PRED_ENDS_WITH, c, "64", 2);
        dftu_expr* up = dftu_expr_str_map(DFTU_STR_MAP_UPPER, c);
        dftu_expr* len = dftu_expr_str_len(c, 1);
        dftu_expr* find = dftu_expr_str_find(c, "d", 1);
        dftu_expr* rep = dftu_expr_str_replace(c, "r", 1, "R", 1, 1);
        dftu_expr* sl = dftu_expr_str_slice(c, 0, 2);
        Series set = Series::strings({"readv"});
        dftu_expr* isin = dftu_expr_is_in(c, set.handle());
        REQUIRE(pred);
        REQUIRE(up);
        REQUIRE(len);
        REQUIRE(find);
        REQUIRE(rep);
        REQUIRE(sl);
        REQUIRE(isin);
        const dftu_series* in[1] = {s.handle()};
        Series r_pred{dftu_expr_eval(pred, in, 1)};
        Series r_up{dftu_expr_eval(up, in, 1)};
        Series r_len{dftu_expr_eval(len, in, 1)};
        Series r_find{dftu_expr_eval(find, in, 1)};
        Series r_rep{dftu_expr_eval(rep, in, 1)};
        Series r_sl{dftu_expr_eval(sl, in, 1)};
        Series r_isin{dftu_expr_eval(isin, in, 1)};
        CHECK(bools(r_pred) ==
              std::vector<bool>{false, true, false, false, false});
        CHECK(strs(r_up)[0] == "READ");
        CHECK(i64s(r_len) == std::vector<std::int64_t>{4, 7, 5, 5, 5});
        CHECK(i64s(r_find) == std::vector<std::int64_t>{3, 4, -1, -1, 3});
        CHECK(strs(r_rep)[0] == "Read");
        CHECK(strs(r_sl) ==
              std::vector<std::string>{"re", "pr", "Wr", "fs", "re"});
        CHECK(bools(r_isin) ==
              std::vector<bool>{false, false, false, false, true});
        CHECK(dftu_expr_str_pred(DFTU_STR_PRED_LIKE, nullptr, "a", 1) ==
              nullptr);
        CHECK(dftu_expr_is_in(c, nullptr) == nullptr);
        for (dftu_expr* e : {pred, up, len, find, rep, sl, isin, c})
            dftu_expr_free(e);
    }

    TEST_CASE("is_null and is_not_null read the validity of the slot") {
        Series s =
            Series::strings({"a-b", "no", "c-d"}).str_extract("(\\w)-(\\w)", 1);
        REQUIRE(s.null_count() == 1);
        std::vector<const Series*> in{&s};
        CHECK(bools(eval(expr_is_null(expr_col(0), true), in)) ==
              std::vector<bool>{false, true, false});
        CHECK(bools(eval(expr_is_null(expr_col(0), false), in)) ==
              std::vector<bool>{true, false, true});
        check_inferred(expr_is_null(expr_col(0), true), s);

        Series full = names();
        std::vector<const Series*> fin{&full};
        CHECK(bools(eval(expr_is_null(expr_col(0), true), fin)) ==
              std::vector<bool>{false, false, false, false, false});

        dftu_expr* c = dftu_expr_col(0);
        dftu_expr* isnull = dftu_expr_is_null(c, 1);
        dftu_expr* notnull = dftu_expr_is_null(c, 0);
        REQUIRE(isnull);
        REQUIRE(notnull);
        const dftu_series* cin[1] = {s.handle()};
        CHECK(bools(Series{dftu_expr_eval(isnull, cin, 1)}) ==
              std::vector<bool>{false, true, false});
        CHECK(bools(Series{dftu_expr_eval(notnull, cin, 1)}) ==
              std::vector<bool>{true, false, true});
        CHECK(dftu_expr_is_null(nullptr, 1) == nullptr);
        for (dftu_expr* e : {isnull, notnull, c}) dftu_expr_free(e);
    }
}
