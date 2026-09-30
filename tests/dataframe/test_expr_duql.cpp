// The duql-semantics Expr nodes: every unknown result is a null cell, and
// numbers compare exactly across integer and double columns.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/duql/pattern_engine.h>
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace df = dftracer::utils::dataframe;
namespace duql = dftracer::utils::duql;
using df::ArithOp;
using df::CmpOp;
using df::ConvertOp;
using df::Expr;
using df::Series;
using df::TypeId;

namespace {

constexpr std::int64_t I64_MAX = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t I64_MIN = std::numeric_limits<std::int64_t>::min();
constexpr std::uint64_t U64_MAX = std::numeric_limits<std::uint64_t>::max();
const double NaN = std::numeric_limits<double>::quiet_NaN();

template <class T>
using Opt = std::vector<std::optional<T>>;

std::vector<std::uint8_t> validity_of(const std::vector<bool>& present) {
    std::vector<std::uint8_t> bits((present.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < present.size(); ++i)
        if (present[i])
            bits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    return bits;
}

template <class T>
Series column(TypeId t, const Opt<T>& rows) {
    std::vector<T> vals;
    std::vector<bool> present;
    for (const auto& r : rows) {
        vals.push_back(r.value_or(T{}));
        present.push_back(r.has_value());
    }
    const auto bits = validity_of(present);
    return Series::flat(t, vals.data(), static_cast<std::int64_t>(vals.size()),
                        bits.data());
}

Series ints(const Opt<std::int64_t>& rows) {
    return column(TypeId::Int64, rows);
}
Series uints(const Opt<std::uint64_t>& rows) {
    return column(TypeId::Uint64, rows);
}
Series dbls(const Opt<double>& rows) { return column(TypeId::Float64, rows); }

Series strs(const Opt<std::string>& rows) {
    std::vector<std::string_view> vals;
    std::vector<bool> present;
    for (const auto& r : rows) {
        vals.push_back(r ? std::string_view(*r) : std::string_view());
        present.push_back(r.has_value());
    }
    const auto bits = validity_of(present);
    return Series::strings(std::span<const std::string_view>(vals),
                           bits.data());
}

Series bools(const Opt<bool>& rows) {
    std::vector<bool> on, present;
    for (const auto& r : rows) {
        on.push_back(r.value_or(false));
        present.push_back(r.has_value());
    }
    const auto data = validity_of(on);
    const auto bits = validity_of(present);
    return Series::flat(TypeId::Bool, data.data(),
                        static_cast<std::int64_t>(rows.size()), bits.data());
}

template <class T>
Opt<T> read(const Series& s, TypeId t) {
    REQUIRE(s.valid());
    REQUIRE(s.type() == t);
    const Series m = s.materialize();
    Opt<T> out;
    for (std::int64_t i = 0; i < m.length(); ++i) {
        if (m.is_null(i)) {
            out.emplace_back();
        } else if constexpr (std::is_same_v<T, bool>) {
            const std::uint8_t* b = m.data<std::uint8_t>();
            out.emplace_back(((b[i >> 3] >> (i & 7)) & 1) != 0);
        } else if constexpr (std::is_same_v<T, std::string>) {
            out.emplace_back(std::string(m.string_at(i)));
        } else {
            out.emplace_back(m.data<T>()[i]);
        }
    }
    return out;
}

Opt<std::int64_t> as_ints(const Series& s) {
    return read<std::int64_t>(s, TypeId::Int64);
}
Opt<double> as_dbls(const Series& s) {
    return read<double>(s, TypeId::Float64);
}
Opt<bool> as_bools(const Series& s) { return read<bool>(s, TypeId::Bool); }
Opt<std::string> as_strs(const Series& s) {
    return read<std::string>(s, TypeId::String);
}

Series run(const Expr& e, const std::vector<const Series*>& in) {
    return df::eval(e, in);
}

dftu_scalar si(std::int64_t v) {
    dftu_scalar s{};
    s.kind = DFTU_SCALAR_TAG_I64;
    s.value.i = v;
    return s;
}
dftu_scalar su(std::uint64_t v) {
    dftu_scalar s{};
    s.kind = DFTU_SCALAR_TAG_U64;
    s.value.u = v;
    return s;
}
dftu_scalar sf(double v) {
    dftu_scalar s{};
    s.kind = DFTU_SCALAR_TAG_F64;
    s.value.d = v;
    return s;
}
dftu_scalar ss(std::string_view v) {
    dftu_scalar s{};
    s.kind = DFTU_SCALAR_TAG_STR;
    s.value.s = v.data();
    s.len = static_cast<std::uint32_t>(v.size());
    return s;
}

Expr c0() { return df::expr_col(0); }
Expr c1() { return df::expr_col(1); }

Opt<std::int64_t> arith_i(ArithOp op, const Series& a, const Series& b) {
    return as_ints(run(df::expr_arith(op, c0(), c1()), {&a, &b}));
}
Opt<double> arith_d(ArithOp op, const Series& a, const Series& b) {
    return as_dbls(run(df::expr_arith(op, c0(), c1()), {&a, &b}));
}

Opt<bool> cmp_col(CmpOp op, const Series& a, dftu_scalar rhs) {
    return as_bools(run(df::expr_cmp(op, c0(), rhs), {&a}));
}

}  // namespace

TEST_CASE("arith on integers stays Int64 and nulls an overflow") {
    const Series a =
        ints({1, I64_MAX, I64_MIN, std::nullopt, std::int64_t{1} << 62});
    const Series b = ints({2, 1, 1, 5, 4});
    CHECK(arith_i(ArithOp::Add, a, b) ==
          Opt<std::int64_t>{3, std::nullopt, I64_MIN + 1, std::nullopt,
                            (std::int64_t{1} << 62) + 4});
    CHECK(arith_i(ArithOp::Sub, a, b) ==
          Opt<std::int64_t>{-1, I64_MAX - 1, std::nullopt, std::nullopt,
                            (std::int64_t{1} << 62) - 4});
    CHECK(arith_i(ArithOp::Mul, a, b) ==
          Opt<std::int64_t>{2, I64_MAX, I64_MIN, std::nullopt, std::nullopt});
}

TEST_CASE("floor division floors and modulo takes the divisor's sign") {
    const Series a = ints({-7, 7, 7, -7, 7, I64_MIN});
    const Series b = ints({2, -2, 0, 3, -3, -1});
    CHECK(arith_i(ArithOp::FloorDiv, a, b) ==
          Opt<std::int64_t>{-4, -4, std::nullopt, -3, -3, std::nullopt});
    CHECK(arith_i(ArithOp::Mod, a, b) ==
          Opt<std::int64_t>{1, -1, std::nullopt, 2, -2, 0});

    const Series x = dbls({-7.0, 7.0, -7.5, 1.0});
    const Series y = dbls({3.0, -3.0, 2.0, 0.0});
    CHECK(arith_d(ArithOp::Mod, x, y) ==
          Opt<double>{2.0, -2.0, 0.5, std::nullopt});
    CHECK(arith_d(ArithOp::FloorDiv, x, y) ==
          Opt<double>{-3.0, -3.0, -4.0, std::nullopt});
}

TEST_CASE("division is Float64 and a zero divisor or a NaN is null") {
    const Series a = ints({7, 1, 0});
    const Series b = ints({2, 0, 0});
    CHECK(arith_d(ArithOp::Div, a, b) ==
          Opt<double>{3.5, std::nullopt, std::nullopt});

    const Series inf = dbls({std::numeric_limits<double>::infinity(), 1.5});
    const Series i2 = ints({0, 1});
    CHECK(arith_d(ArithOp::Mul, inf, i2) == Opt<double>{std::nullopt, 1.5});
    CHECK(arith_d(ArithOp::Add, inf, i2) ==
          Opt<double>{std::numeric_limits<double>::infinity(), 2.5});
}

TEST_CASE("arith mixes Uint64, narrow integers and literals") {
    const Series u = uints({std::uint64_t{1} << 63, U64_MAX, 3});
    const Series i = ints({-1, 0, -5});
    CHECK(arith_i(ArithOp::Add, u, i) ==
          Opt<std::int64_t>{I64_MAX, std::nullopt, -2});

    const std::vector<std::int32_t> n32{100, -3};
    const std::vector<std::int8_t> n8{27, 4};
    const Series a = Series::flat(TypeId::Int32, n32.data(), 2);
    const Series b = Series::flat(TypeId::Int8, n8.data(), 2);
    CHECK(arith_i(ArithOp::Add, a, b) == Opt<std::int64_t>{127, 1});

    const Series c = ints({5, 6});
    CHECK(as_ints(
              run(df::expr_arith(ArithOp::Sub, df::lit(std::int64_t{10}), c0()),
                  {&c})) == Opt<std::int64_t>{5, 4});
    CHECK(as_dbls(run(df::expr_arith(ArithOp::Add, c0(), df::lit(0.5)),
                      {&c})) == Opt<double>{5.5, 6.5});

    const Series s = strs({"a", "b"});
    CHECK_THROWS_AS(run(df::expr_arith(ArithOp::Add, c0(), c0()), {&s}),
                    std::invalid_argument);
}

TEST_CASE("neg nulls INT64_MIN and a uint64 past it") {
    const Series a = ints({5, I64_MIN, std::nullopt});
    CHECK(as_ints(run(df::expr_neg(c0()), {&a})) ==
          Opt<std::int64_t>{-5, std::nullopt, std::nullopt});
    const Series u = uints({std::uint64_t{1} << 63, U64_MAX});
    CHECK(as_ints(run(df::expr_neg(c0()), {&u})) ==
          Opt<std::int64_t>{I64_MIN, std::nullopt});
    const Series d = dbls({1.5});
    CHECK(as_dbls(run(df::expr_neg(c0()), {&d})) == Opt<double>{-1.5});
}

TEST_CASE("cmp_expr compares integers and doubles exactly") {
    const Series i = ints({9007199254740993, 2, 3, std::nullopt, 1});
    const Series d = dbls({9007199254740992.0, 2.0, 2.5, 1.0, NaN});
    auto cmp = [&](CmpOp op, const Expr& a, const Expr& b) {
        return as_bools(run(df::expr_cmp_expr(op, a, b), {&i, &d}));
    };
    CHECK(cmp(CmpOp::Gt, c0(), c1()) ==
          Opt<bool>{true, false, true, std::nullopt, std::nullopt});
    CHECK(cmp(CmpOp::Eq, c0(), c1()) ==
          Opt<bool>{false, true, false, std::nullopt, std::nullopt});
    CHECK(cmp(CmpOp::Lt, c1(), c0()) ==
          Opt<bool>{true, false, true, std::nullopt, std::nullopt});

    const Series u = uints({U64_MAX, 0});
    const Series s = ints({-1, 0});
    CHECK(as_bools(run(df::expr_cmp_expr(CmpOp::Gt, c0(), c1()), {&u, &s})) ==
          Opt<bool>{true, false});
}

TEST_CASE("cmp_expr orders strings bytewise and false before true") {
    const Series a = strs({"a", "B", "\xc3\xa9", std::nullopt});
    const Series b = strs({"b", "a", "z", "x"});
    CHECK(as_bools(run(df::expr_cmp_expr(CmpOp::Lt, c0(), c1()), {&a, &b})) ==
          Opt<bool>{true, true, false, std::nullopt});
    const Series x = bools({false, true, true});
    const Series y = bools({true, false, true});
    CHECK(as_bools(run(df::expr_cmp_expr(CmpOp::Lt, c0(), c1()), {&x, &y})) ==
          Opt<bool>{true, false, false});
    CHECK_THROWS_AS(run(df::expr_cmp_expr(CmpOp::Eq, c0(), c1()), {&a, &x}),
                    std::invalid_argument);
}

TEST_CASE("expr_cmp against a scalar is exact across integer and double") {
    const Series big = ints({9007199254740993, 9007199254740992});
    CHECK(cmp_col(CmpOp::Gt, big, sf(9007199254740992.0)) ==
          Opt<bool>{true, false});
    CHECK(cmp_col(CmpOp::Eq, big, sf(9007199254740992.0)) ==
          Opt<bool>{false, true});

    const Series i = ints({2, 3});
    CHECK(cmp_col(CmpOp::Ge, i, sf(2.5)) == Opt<bool>{false, true});
    CHECK(cmp_col(CmpOp::Lt, i, sf(2.5)) == Opt<bool>{true, false});
    CHECK(cmp_col(CmpOp::Eq, i, sf(2.5)) == Opt<bool>{false, false});
    CHECK(cmp_col(CmpOp::Ne, i, sf(2.5)) == Opt<bool>{true, true});
    CHECK(cmp_col(CmpOp::Lt, i, sf(1e300)) == Opt<bool>{true, true});
    CHECK(cmp_col(CmpOp::Gt, i, sf(-1e300)) == Opt<bool>{true, true});
    CHECK(cmp_col(CmpOp::Ne, i, sf(NaN)) == Opt<bool>{true, true});
    CHECK(cmp_col(CmpOp::Eq, i, sf(NaN)) == Opt<bool>{false, false});

    const Series d = dbls({9007199254740992.0});
    CHECK(cmp_col(CmpOp::Lt, d, si(9007199254740993)) == Opt<bool>{true});
    CHECK(cmp_col(CmpOp::Eq, d, si(9007199254740993)) == Opt<bool>{false});
    CHECK(cmp_col(CmpOp::Ge, d, si(9007199254740993)) == Opt<bool>{false});

    const std::vector<std::int8_t> n8{100, -100};
    const Series s8 = Series::flat(TypeId::Int8, n8.data(), 2);
    CHECK(cmp_col(CmpOp::Lt, s8, si(300)) == Opt<bool>{true, true});
    CHECK(cmp_col(CmpOp::Gt, s8, si(-300)) == Opt<bool>{true, true});

    const Series u = uints({0, 5});
    CHECK(cmp_col(CmpOp::Gt, u, si(-1)) == Opt<bool>{true, true});
    CHECK(cmp_col(CmpOp::Eq, i, su(U64_MAX)) == Opt<bool>{false, false});

    const std::vector<float> f{0.1f};
    const Series f32 = Series::flat(TypeId::Float32, f.data(), 1);
    CHECK(cmp_col(CmpOp::Le, f32, sf(0.1)) == Opt<bool>{false});
    CHECK(cmp_col(CmpOp::Gt, f32, sf(0.1)) == Opt<bool>{true});
    CHECK(cmp_col(CmpOp::Eq, f32, sf(0.1)) == Opt<bool>{false});
    CHECK(cmp_col(CmpOp::Eq, f32, sf(static_cast<double>(0.1f))) ==
          Opt<bool>{true});
}

TEST_CASE("coalesce takes the first present operand in order") {
    const Series a = ints({std::nullopt, 2, std::nullopt});
    const Series b = ints({1, 7, std::nullopt});
    CHECK(as_ints(run(df::expr_coalesce({c0(), c1(), df::lit(std::int64_t{9})}),
                      {&a, &b})) == Opt<std::int64_t>{1, 2, 9});
    CHECK(as_ints(run(df::expr_coalesce({c1(), c0()}), {&a, &b})) ==
          Opt<std::int64_t>{1, 7, std::nullopt});

    const Series d = dbls({0.5, 0.25, std::nullopt});
    CHECK(as_dbls(run(df::expr_coalesce({c0(), c1()}), {&a, &d})) ==
          Opt<double>{0.5, 2.0, std::nullopt});

    const Series u = uints({U64_MAX, std::nullopt});
    const Series i = ints({1, 2});
    CHECK(as_ints(run(df::expr_coalesce({c0(), c1()}), {&u, &i})) ==
          Opt<std::int64_t>{std::nullopt, 2});

    const Series s = strs({std::nullopt, "x"});
    const Series t = strs({"y", "z"});
    CHECK(as_strs(run(df::expr_coalesce({c0(), c1()}), {&s, &t})) ==
          Opt<std::string>{"y", "x"});
    CHECK_THROWS_AS(run(df::expr_coalesce({c0(), c1()}), {&s, &i}),
                    std::invalid_argument);
    CHECK_THROWS_AS(df::expr_coalesce({}), std::invalid_argument);
}

TEST_CASE("least and greatest are null when any operand is null") {
    const Series a = ints({3, std::nullopt, 5});
    const Series b = ints({4, 1, 2});
    CHECK(as_ints(run(df::expr_extreme({c0(), c1()}, true), {&a, &b})) ==
          Opt<std::int64_t>{3, std::nullopt, 2});
    CHECK(as_ints(run(
              df::expr_extreme({c0(), c1(), df::lit(std::int64_t{4})}, false),
              {&a, &b})) == Opt<std::int64_t>{4, std::nullopt, 5});

    const Series u = uints({U64_MAX});
    const Series i = ints({-1});
    CHECK(as_ints(run(df::expr_extreme({c0(), c1()}, true), {&u, &i})) ==
          Opt<std::int64_t>{-1});
    CHECK(as_ints(run(df::expr_extreme({c0(), c1()}, false), {&u, &i})) ==
          Opt<std::int64_t>{std::nullopt});

    const Series big = ints({9007199254740993});
    const Series d = dbls({9007199254740992.0});
    CHECK(as_dbls(run(df::expr_extreme({c0(), c1()}, true), {&big, &d})) ==
          Opt<double>{9007199254740992.0});
    const Series nan = dbls({NaN});
    CHECK(as_dbls(run(df::expr_extreme({c0(), c1()}, true), {&d, &nan})) ==
          Opt<double>{std::nullopt});

    const Series s = strs({"b", "B"});
    const Series t = strs({"a", "a"});
    CHECK(as_strs(run(df::expr_extreme({c0(), c1()}, true), {&s, &t})) ==
          Opt<std::string>{"a", "B"});
}

TEST_CASE("concat joins strings and is null when any operand is null") {
    const Series a = strs({"a", std::nullopt, ""});
    const Series b = strs({"b", "c", "d"});
    CHECK(as_strs(run(df::expr_concat({c0(), c1(), df::expr_lit_str("!")}),
                      {&a, &b})) ==
          Opt<std::string>{"ab!", std::nullopt, "d!"});
    const Series i = ints({1, 2, 3});
    CHECK_THROWS_AS(run(df::expr_concat({c0(), c1()}), {&a, &i}),
                    std::invalid_argument);
}

TEST_CASE("round is half away from zero and keeps integers") {
    const Series d = dbls({2.5, -2.5, 1.2345, 15.0, std::nullopt});
    CHECK(as_dbls(run(df::expr_round(c0(), 0), {&d})) ==
          Opt<double>{3.0, -3.0, 1.0, 15.0, std::nullopt});
    CHECK(as_dbls(run(df::expr_round(c0(), 2), {&d}))[2] ==
          std::optional<double>(1.23));
    CHECK(as_dbls(run(df::expr_round(c0(), -1), {&d})) ==
          Opt<double>{0.0, -0.0, 0.0, 20.0, std::nullopt});
    CHECK(as_dbls(run(df::expr_round(c0(), 400), {&d})) ==
          Opt<double>{std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                      std::nullopt});

    const std::vector<std::int32_t> n32{1234, -1250};
    const Series i32 = Series::flat(TypeId::Int32, n32.data(), 2);
    const Series same = run(df::expr_round(c0(), 2), {&i32});
    CHECK(same.type() == TypeId::Int32);
    CHECK(as_ints(run(df::expr_round(c0(), -2), {&i32})) ==
          Opt<std::int64_t>{1200, -1300});
}

TEST_CASE("log and pow give null instead of NaN") {
    const Series a = dbls({std::exp(1.0), 0.0, -1.0, std::nullopt});
    CHECK(as_dbls(run(df::expr_log(c0()), {&a})) ==
          Opt<double>{1.0, std::nullopt, std::nullopt, std::nullopt});
    const Series x = dbls({-8.0, 2.0, 0.0});
    const Series y = dbls({1.0 / 3.0, 10.0, -1.0});
    CHECK(as_dbls(run(df::expr_pow(c0(), c1()), {&x, &y})) ==
          Opt<double>{std::nullopt, 1024.0,
                      std::numeric_limits<double>::infinity()});
    const Series i = ints({2});
    CHECK(as_dbls(run(df::expr_pow(c0(), df::lit(std::int64_t{3})), {&i})) ==
          Opt<double>{8.0});
}

TEST_CASE("substr counts UTF-8 characters") {
    const Series s = strs({"h\xc3\xa9llo", "ab", std::nullopt});
    CHECK(as_strs(run(df::expr_str_substr(c0(), 1, 3), {&s})) ==
          Opt<std::string>{"\xc3\xa9ll", "b", std::nullopt});
    CHECK(as_strs(run(df::expr_str_substr(c0(), 2, -1), {&s})) ==
          Opt<std::string>{"llo", "", std::nullopt});
    CHECK(as_strs(run(df::expr_str_substr(c0(), 10, 2), {&s})) ==
          Opt<std::string>{"", "", std::nullopt});
    CHECK(as_strs(run(df::expr_str_substr(c0(), -1, 2), {&s})) ==
          Opt<std::string>{std::nullopt, std::nullopt, std::nullopt});
}

TEST_CASE("str_pattern runs a compiled duql pattern; a work limit is null") {
    const Series s = strs({"hello", "HELLO", "world", std::nullopt});
    auto ilike = duql::compile_like("HE%", true, std::nullopt);
    REQUIRE(ilike);
    CHECK(as_bools(run(df::expr_str_pattern(c0(), *ilike), {&s})) ==
          Opt<bool>{true, true, false, std::nullopt});
    auto icontains = duql::compile_contains("LL", true);
    REQUIRE(icontains);
    CHECK(as_bools(run(df::expr_str_pattern(c0(), *icontains), {&s})) ==
          Opt<bool>{true, true, false, std::nullopt});

    const Series hard = strs({std::string(10000, 'a') + "!", "AAA"});
    auto iregex = duql::compile_regex("^(a|aa)+$", true);
    REQUIRE(iregex);
#ifdef DFTRACER_UTILS_ENABLE_VECTORSCAN
    CHECK(as_bools(run(df::expr_str_pattern(c0(), *iregex), {&hard})) ==
          Opt<bool>{false, true});
#else
    CHECK(as_bools(run(df::expr_str_pattern(c0(), *iregex), {&hard})) ==
          Opt<bool>{std::nullopt, true});
#endif

    CHECK_THROWS_AS(run(df::expr_str_pattern(c0(), nullptr), {&s}),
                    std::invalid_argument);
}

TEST_CASE("str_extract returns a capture group or null") {
    const Series s = strs({"12-34", "x", std::nullopt});
    auto re = duql::compile_regex("(\\d+)-(\\d+)", false);
    REQUIRE(re);
    CHECK(as_strs(run(df::expr_str_extract(c0(), *re, 2), {&s})) ==
          Opt<std::string>{"34", std::nullopt, std::nullopt});
    CHECK(as_strs(run(df::expr_str_extract(c0(), *re, 0), {&s})) ==
          Opt<std::string>{"12-34", std::nullopt, std::nullopt});
    CHECK_THROWS_AS(run(df::expr_str_extract(c0(), *re, 3), {&s}),
                    std::invalid_argument);

    const Series hard = strs({std::string(10000, 'a') + "!"});
    auto slow = duql::compile_regex("^(a|aa)+$", false);
    REQUIRE(slow);
    CHECK(as_strs(run(df::expr_str_extract(c0(), *slow, 1), {&hard})) ==
          Opt<std::string>{std::nullopt});
}

TEST_CASE("string predicates take a column needle") {
    const Series s = strs({"hello", "hello", "", "abc", std::nullopt, "x"});
    const Series n = strs({"he", "lo", "", "bc", "a", std::nullopt});
    using P = df::StrPredOp;
    CHECK(as_bools(run(df::expr_str_pred_col(P::StartsWith, c0(), c1()),
                       {&s, &n})) ==
          Opt<bool>{true, false, true, false, std::nullopt, std::nullopt});
    CHECK(as_bools(
              run(df::expr_str_pred_col(P::EndsWith, c0(), c1()), {&s, &n})) ==
          Opt<bool>{false, true, true, true, std::nullopt, std::nullopt});
    CHECK(as_bools(
              run(df::expr_str_pred_col(P::Contains, c0(), c1()), {&s, &n})) ==
          Opt<bool>{true, true, true, true, std::nullopt, std::nullopt});
    const Series longer = strs({"hello world", "a", "", "abcd", "q", "x"});
    CHECK(as_bools(run(df::expr_str_pred_col(P::Contains, c0(), c1()),
                       {&s, &longer})) ==
          Opt<bool>{false, false, true, false, std::nullopt, true});
    for (P op : {P::Contains, P::StartsWith, P::EndsWith}) {
        CHECK(
            as_bools(run(df::expr_str_pred_col(op, c0(), df::expr_lit_str("l")),
                         {&s})) ==
            as_bools(run(df::expr_str_pred(op, c0(), "l"), {&s})));
    }
    CHECK_THROWS_AS(df::expr_str_pred_col(P::Like, c0(), c1()),
                    std::invalid_argument);
    CHECK(df::infer_type(df::expr_str_pred_col(P::Contains, c0(), c1()),
                         {s.data_type(), n.data_type()})
              .id == TypeId::Bool);
}

TEST_CASE("replace takes column from and to") {
    const Series s = strs({"aXbXc", "abc", "aaa", "abc", std::nullopt, "xyz"});
    const Series from = strs({"X", "", "aa", "b", "a", std::nullopt});
    const Series to = strs({"--", "Z", "Q", "", "a", "a"});
    CHECK(as_strs(run(df::expr_str_replace_col(c0(), c1(), df::expr_col(2)),
                      {&s, &from, &to})) ==
          Opt<std::string>{"a--b--c", "abc", "Qa", "ac", std::nullopt,
                           std::nullopt});
    CHECK(
        as_strs(run(df::expr_str_replace_col(c0(), df::expr_lit_str("b"), c1()),
                    {&s, &to})) ==
        Opt<std::string>{"aX--Xc", "aZc", "aaa", "ac", std::nullopt, "xyz"});
    CHECK(
        as_strs(run(df::expr_str_replace_col(c0(), df::expr_lit_str("a"),
                                             df::expr_lit_str("\xc3\xa9")),
                    {&s})) ==
        as_strs(run(df::expr_str_replace(c0(), "a", "\xc3\xa9", true), {&s})));
}

TEST_CASE("substr takes column start and length") {
    const Series s =
        strs({"h\xc3\xa9llo", "abc", "abc", "abc", std::nullopt, "abc"});
    const Series start = ints({1, 0, -1, 10, 0, std::nullopt});
    const Series len = ints({3, -1, 2, 2, 1, 1});
    CHECK(as_strs(run(df::expr_str_substr_col(c0(), c1(), nullptr),
                      {&s, &start})) ==
          Opt<std::string>{"\xc3\xa9llo", "abc", std::nullopt, "", std::nullopt,
                           std::nullopt});
    const Expr l = df::expr_col(2);
    CHECK(as_strs(run(df::expr_str_substr_col(c0(), c1(), &l),
                      {&s, &start, &len})) ==
          Opt<std::string>{"\xc3\xa9ll", std::nullopt, std::nullopt, "",
                           std::nullopt, std::nullopt});
    const Expr lit_len = df::lit(std::int64_t{2});
    CHECK(as_strs(run(df::expr_str_substr_col(c0(), c1(), &lit_len),
                      {&s, &start})) ==
          Opt<std::string>{"\xc3\xa9l", "ab", std::nullopt, "", std::nullopt,
                           std::nullopt});
    const Series fl = dbls({1.0, 1.0, 1.0, 1.0, 1.0, 1.0});
    CHECK(
        as_strs(run(df::expr_str_substr_col(c0(), c1(), nullptr), {&s, &fl})) ==
        Opt<std::string>{std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                         std::nullopt, std::nullopt});
}

TEST_CASE("round takes a column of digits") {
    const Series d = dbls({2.5, -2.5, 1.2345, 15.0, std::nullopt, 7.5, 1.5});
    const Series dg = ints({0, 0, 2, -1, 1, std::nullopt, 400});
    CHECK(as_dbls(run(df::expr_round_col(c0(), c1()), {&d, &dg})) ==
          Opt<double>{3.0, -3.0, 1.23, 20.0, std::nullopt, std::nullopt,
                      std::nullopt});
    const Series n = ints({1234, -1250, 15, 15, std::nullopt, 15, 15});
    const Series dg2 = ints({2, -2, 0, -1, 0, 0, 400});
    CHECK(as_ints(run(df::expr_round_col(c0(), c1()), {&n, &dg2})) ==
          Opt<std::int64_t>{1234, -1300, 15, 20, std::nullopt, 15, 15});
    const Series fd = dbls({0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0});
    CHECK(as_ints(run(df::expr_round_col(c0(), c1()), {&n, &fd})) ==
          Opt<std::int64_t>(7, std::nullopt));
    const std::uint64_t big = std::uint64_t{1} << 63;
    const Series u = uints({big + 2048, big + 2048, U64_MAX, 15, std::nullopt});
    const Series ud = ints({0, -2, -1, -1, 0});
    const auto ur = read<std::uint64_t>(
        run(df::expr_round_col(c0(), c1()), {&u, &ud}), TypeId::Uint64);
    CHECK(ur == Opt<std::uint64_t>{big + 2048, std::nullopt, std::nullopt, 20,
                                   std::nullopt});
    CHECK(as_dbls(run(df::expr_round_col(c0(), df::lit(std::int64_t{2})),
                      {&d})) == as_dbls(run(df::expr_round(c0(), 2), {&d})));
    CHECK(df::infer_type(df::expr_round_col(c0(), c1()),
                         {n.data_type(), dg2.data_type()})
              .id == TypeId::Int64);
}

TEST_CASE("extract takes a column of groups") {
    const Series s =
        strs({"12-34", "12-34", "12-34", "12-34", "x", std::nullopt, "12-34"});
    const Series g = ints({2, 0, 3, -1, 1, 1, std::nullopt});
    auto re = duql::compile_regex("(\\d+)-(\\d+)", false);
    REQUIRE(re);
    CHECK(as_strs(run(df::expr_str_extract_col(c0(), *re, c1()), {&s, &g})) ==
          Opt<std::string>{"34", "12-34", std::nullopt, std::nullopt,
                           std::nullopt, std::nullopt, std::nullopt});
    CHECK(as_strs(run(
              df::expr_str_extract_col(c0(), *re, df::lit(std::int64_t{1})),
              {&s})) == as_strs(run(df::expr_str_extract(c0(), *re, 1), {&s})));
    CHECK_THROWS_AS(
        run(df::expr_str_extract_col(c0(), nullptr, c1()), {&s, &g}),
        std::invalid_argument);
}

TEST_CASE("str_regex_replace substitutes groups and keeps nulls") {
    const Series s = strs({"open64_17", "none", std::nullopt, "/a//b"});
    const std::string_view pat = R"re((?<op>[a-z]+)64_(\d+))re";
    auto re = duql::compile_regex(pat, false);
    REQUIRE(re);
    auto sub = duql::compile_substitution(**re, "${op}#$2$$");
    REQUIRE(sub);
    const Opt<std::string> want{"open#17$", "none", std::nullopt, "/a//b"};
    CHECK(as_strs(run(df::expr_str_regex_replace(c0(), *re, *sub), {&s})) ==
          want);
    CHECK(as_strs(Series{dftu_series_str_regex_replace(
              s.handle(), pat.data(), static_cast<std::int32_t>(pat.size()),
              "${op}#$2$$", 10)}) == want);
    CHECK(as_strs(s.str_regex_replace(pat, "${op}#$2$$")) == want);

    dftu_expr* col = dftu_expr_col(0);
    dftu_expr* e = dftu_expr_str_fn(
        DFTU_STR_FN_REGEX_REPLACE, col, nullptr, pat.data(),
        static_cast<std::int32_t>(pat.size()), "${op}#$2$$", 10, 0, 0);
    REQUIRE(e);
    const dftu_series* in[1] = {s.handle()};
    CHECK(as_strs(Series{dftu_expr_eval(e, in, 1)}) == want);
    dftu_expr_free(e);
    dftu_expr_free(col);

    CHECK_THROWS_AS(run(df::expr_str_regex_replace(c0(), nullptr, *sub), {&s}),
                    std::invalid_argument);
    CHECK_FALSE(duql::compile_substitution(**re, "$3"));
    CHECK_FALSE(duql::compile_substitution(**re, "$x"));
    CHECK_FALSE(s.str_regex_replace("(", "x").valid());
}

TEST_CASE("convert to int parses, truncates and nulls the unknown") {
    const Series s = strs({"42", "-7", "3.9", "abc", "1e3",
                           "99999999999999999999", "", std::nullopt});
    CHECK(as_ints(run(df::expr_convert(ConvertOp::Int, c0()), {&s})) ==
          Opt<std::int64_t>{42, -7, 3, std::nullopt, 1000, std::nullopt,
                            std::nullopt, std::nullopt});
    const Series d = dbls({2.9, -2.9, NaN, 1e300});
    CHECK(as_ints(run(df::expr_convert(ConvertOp::Int, c0()), {&d})) ==
          Opt<std::int64_t>{2, -2, std::nullopt, std::nullopt});
    const Series b = bools({true, false, std::nullopt});
    CHECK(as_ints(run(df::expr_convert(ConvertOp::Int, c0()), {&b})) ==
          Opt<std::int64_t>{1, 0, std::nullopt});
    const Series u = uints({U64_MAX, 5});
    CHECK(as_ints(run(df::expr_convert(ConvertOp::Int, c0()), {&u})) ==
          Opt<std::int64_t>{std::nullopt, 5});
}

TEST_CASE("convert to float parses and nulls NaN") {
    const Series s = strs({"1.5", "x", "nan", "inf"});
    CHECK(as_dbls(run(df::expr_convert(ConvertOp::Float, c0()), {&s})) ==
          Opt<double>{1.5, std::nullopt, std::nullopt,
                      std::numeric_limits<double>::infinity()});
    const Series i = ints({-3, std::nullopt});
    CHECK(as_dbls(run(df::expr_convert(ConvertOp::Float, c0()), {&i})) ==
          Opt<double>{-3.0, std::nullopt});
    const Series b = bools({true});
    CHECK(as_dbls(run(df::expr_convert(ConvertOp::Float, c0()), {&b})) ==
          Opt<double>{1.0});
}

TEST_CASE("convert to string and json write duql's text forms") {
    const Series i = ints({-5, std::nullopt});
    CHECK(as_strs(run(df::expr_convert(ConvertOp::String, c0()), {&i})) ==
          Opt<std::string>{"-5", std::nullopt});
    CHECK(as_strs(run(df::expr_convert(ConvertOp::Json, c0()), {&i})) ==
          Opt<std::string>{"-5", "null"});

    const Series d = dbls({0.1, 1.0, 1e21, NaN});
    CHECK(as_strs(run(df::expr_convert(ConvertOp::String, c0()), {&d})) ==
          Opt<std::string>{"0.1", "1", "1e+21", std::nullopt});
    CHECK(as_strs(run(df::expr_convert(ConvertOp::Json, c0()), {&d})) ==
          Opt<std::string>{"0.1", "1", "1e+21", "null"});

    const Series b = bools({true, false});
    CHECK(as_strs(run(df::expr_convert(ConvertOp::String, c0()), {&b})) ==
          Opt<std::string>{"true", "false"});

    const Series s = strs({"a\"b\n", std::nullopt});
    CHECK(as_strs(run(df::expr_convert(ConvertOp::Json, c0()), {&s})) ==
          Opt<std::string>{"\"a\\\"b\\n\"", "null"});
    CHECK(as_strs(run(df::expr_convert(ConvertOp::String, c0()), {&s})) ==
          Opt<std::string>{"a\"b\n", std::nullopt});

    const Series u = uints({U64_MAX});
    CHECK(as_strs(run(df::expr_convert(ConvertOp::String, c0()), {&u})) ==
          Opt<std::string>{"18446744073709551615"});
}

namespace {

// A List column; row i is null where !present[i] (a take of -1).
Series make_list(const std::vector<std::int32_t>& offsets, Series values,
                 const std::vector<bool>& present) {
    const Series list = Series::list(offsets, std::move(values));
    std::vector<std::int64_t> idx;
    for (std::size_t i = 0; i < present.size(); ++i)
        idx.push_back(present[i] ? static_cast<std::int64_t>(i) : -1);
    return list.take(idx).materialize();
}

}  // namespace

TEST_CASE("list functions") {
    const Series l =
        make_list({0, 3, 3, 3, 5}, ints({1, 2, 3, 4, std::nullopt}),
                  {true, true, false, true});
    REQUIRE(l.valid());
    CHECK(as_ints(run(df::expr_list_len(c0()), {&l})) ==
          Opt<std::int64_t>{3, 0, std::nullopt, 2});
    CHECK(as_ints(run(df::expr_list_get(c0(), 0), {&l})) ==
          Opt<std::int64_t>{1, std::nullopt, std::nullopt, 4});
    CHECK(as_ints(run(df::expr_list_get(c0(), -1), {&l})) ==
          Opt<std::int64_t>{3, std::nullopt, std::nullopt, std::nullopt});
    CHECK(as_ints(run(df::expr_list_get(c0(), -4), {&l})) ==
          Opt<std::int64_t>{std::nullopt, std::nullopt, std::nullopt,
                            std::nullopt});
    CHECK(as_ints(run(df::expr_list_get(c0(), 5), {&l})) ==
          Opt<std::int64_t>{std::nullopt, std::nullopt, std::nullopt,
                            std::nullopt});
    CHECK(as_ints(run(df::expr_list_sum(c0()), {&l})) ==
          Opt<std::int64_t>{6, 0, std::nullopt, std::nullopt});
    CHECK(as_bools(run(df::expr_list_contains(c0(), si(2)), {&l})) ==
          Opt<bool>{true, false, std::nullopt, false});
    CHECK(as_bools(run(df::expr_list_contains(c0(), sf(4.0)), {&l})) ==
          Opt<bool>{false, false, std::nullopt, true});
    CHECK(as_bools(run(df::expr_list_contains(c0(), ss("1")), {&l})) ==
          Opt<bool>{false, false, std::nullopt, false});

    const Series big = make_list({0, 2}, ints({I64_MAX, 1}), {true});
    CHECK(as_ints(run(df::expr_list_sum(c0()), {&big})) ==
          Opt<std::int64_t>{std::nullopt});

    const Series fl =
        make_list({0, 2, 3}, dbls({0.5, 0.25, NaN}), {true, true});
    CHECK(as_dbls(run(df::expr_list_sum(c0()), {&fl})) ==
          Opt<double>{0.75, std::nullopt});

    const Series sl = make_list({0, 1, 1}, strs({"a"}), {true, true});
    CHECK(as_ints(run(df::expr_list_sum(c0()), {&sl})) ==
          Opt<std::int64_t>{std::nullopt, 0});
    CHECK(as_bools(run(df::expr_list_contains(c0(), ss("a")), {&sl})) ==
          Opt<bool>{true, false});

    const Series i = ints({1});
    CHECK_THROWS_AS(run(df::expr_list_len(c0()), {&i}), std::invalid_argument);
}

TEST_CASE("literals broadcast to a column of their type") {
    const Series i = ints({1, 2});
    CHECK(as_strs(run(df::expr_lit_str("x"), {&i})) ==
          Opt<std::string>{"x", "x"});
    CHECK(as_bools(run(df::expr_lit_bool(true), {&i})) ==
          Opt<bool>{true, true});
    CHECK(as_ints(run(df::expr_lit_null(TypeId::Int64), {&i})) ==
          Opt<std::int64_t>{std::nullopt, std::nullopt});
    CHECK(as_ints(run(df::expr_arith(ArithOp::Add, c0(),
                                     df::expr_lit_null(TypeId::Int64)),
                      {&i})) == Opt<std::int64_t>{std::nullopt, std::nullopt});
    CHECK_THROWS_AS(run(df::expr_lit_null(TypeId::List), {&i}),
                    std::invalid_argument);
}

TEST_CASE("infer_type agrees with the node types") {
    const std::vector<df::DataType> in{df::scalar(TypeId::Int32),
                                       df::scalar(TypeId::Float64)};
    CHECK(df::infer_type(df::expr_arith(ArithOp::Add, c0(), c0()), in).id ==
          TypeId::Int64);
    CHECK(df::infer_type(df::expr_arith(ArithOp::Div, c0(), c0()), in).id ==
          TypeId::Float64);
    CHECK(df::infer_type(df::expr_extreme({c0(), c1()}, true), in).id ==
          TypeId::Float64);
    CHECK(df::infer_type(df::expr_convert(ConvertOp::Json, c0()), in).id ==
          TypeId::String);
}

TEST_CASE("C ABI builders") {
    const Series a = ints({-7, 7});
    const Series b = ints({2, 0});
    dftu_expr* x = dftu_expr_col(0);
    dftu_expr* y = dftu_expr_col(1);
    dftu_expr* fd = dftu_expr_arith(4, x, y);
    REQUIRE(fd != nullptr);
    const dftu_series* in[] = {a.handle(), b.handle()};
    Series out{dftu_expr_eval(fd, in, 2)};
    CHECK(as_ints(out) == Opt<std::int64_t>{-4, std::nullopt});

    const dftu_expr* args[] = {y, x};
    dftu_expr* co = dftu_expr_coalesce(args, 2);
    REQUIRE(co != nullptr);
    CHECK(dftu_expr_coalesce(args, 0) == nullptr);
    CHECK(dftu_expr_arith(0, x, nullptr) == nullptr);
    CHECK(dftu_expr_lit_str(nullptr, 3) == nullptr);

    dftu_expr* s = dftu_expr_lit_str("ab", 2);
    dftu_expr* js = dftu_expr_convert(3, s);
    Series text{dftu_expr_eval(js, in, 2)};
    CHECK(as_strs(text) == Opt<std::string>{"\"ab\"", "\"ab\""});

    for (dftu_expr* e : {x, y, fd, co, s, js}) dftu_expr_free(e);
}

TEST_CASE("kernels run across evaluation chunks") {
    constexpr std::int64_t N = 200000;
    Opt<std::int64_t> a, b;
    for (std::int64_t i = 0; i < N; ++i) {
        a.emplace_back(i % 7 == 0 ? std::nullopt : std::optional(-i));
        b.emplace_back(i % 5);
    }
    const Series sa = ints(a);
    const Series sb = ints(b);
    const auto out = arith_i(ArithOp::Mod, sa, sb);
    REQUIRE(out.size() == static_cast<std::size_t>(N));
    for (std::int64_t i = 0; i < N; ++i) {
        const auto& got = out[static_cast<std::size_t>(i)];
        if (i % 7 == 0 || i % 5 == 0) {
            CHECK_FALSE(got.has_value());
        } else {
            const std::int64_t d = i % 5;
            const std::int64_t r = ((-i % d) + d) % d;
            if (!got || *got != r) FAIL("row " << i);
        }
    }
}

TEST_CASE("string kernels run across evaluation chunks") {
    constexpr std::int64_t N = 200000;
    const Series s = strs(Opt<std::string>(N, std::string("h\xc3\xa9y")));
    const auto sub = as_strs(run(df::expr_str_substr(c0(), 1, 1), {&s}));
    CHECK(sub.front() == std::optional<std::string>("\xc3\xa9"));
    CHECK(sub.back() == std::optional<std::string>("\xc3\xa9"));
}

namespace {

constexpr std::int64_t NS_PER_US = 1'000;
constexpr std::int64_t NS_PER_MS = 1'000'000;
constexpr std::int64_t NS_PER_S = 1'000'000'000;
constexpr std::int64_t NS_PER_MIN = 60 * NS_PER_S;
constexpr std::int64_t NS_PER_H = 3600 * NS_PER_S;
constexpr std::int64_t NS_PER_D = 86400 * NS_PER_S;

Opt<std::int64_t> part(const Series& s, std::int32_t p, std::int64_t unit) {
    return as_ints(run(df::expr_date_part(c0(), p, unit), {&s}));
}
Opt<std::string> fmt(const Series& s, std::string_view f, std::int64_t unit) {
    return as_strs(run(df::expr_format_time(c0(), f, unit), {&s}));
}

}  // namespace

TEST_CASE("date_part reads a time in every unit") {
    // 2023-11-14 22:13:20 UTC, in each unit (coarse units floor).
    const std::int64_t units[7] = {1,          NS_PER_US, NS_PER_MS, NS_PER_S,
                                   NS_PER_MIN, NS_PER_H,  NS_PER_D};
    const std::int64_t values[7] = {1'700'000'000'000'000'000,
                                    1'700'000'000'000'000,
                                    1'700'000'000'000,
                                    1'700'000'000,
                                    28'333'333,
                                    472'222,
                                    19'675};
    const std::int64_t hour[7] = {22, 22, 22, 22, 22, 22, 0};
    const std::int64_t minute[7] = {13, 13, 13, 13, 13, 0, 0};
    const std::int64_t second[7] = {20, 20, 20, 20, 0, 0, 0};
    for (int i = 0; i < 7; ++i) {
        const Series s = ints({values[i]});
        CAPTURE(units[i]);
        CHECK(part(s, DFTU_DT_YEAR, units[i]) == Opt<std::int64_t>{2023});
        CHECK(part(s, DFTU_DT_MONTH, units[i]) == Opt<std::int64_t>{11});
        CHECK(part(s, DFTU_DT_DAY, units[i]) == Opt<std::int64_t>{14});
        CHECK(part(s, DFTU_DT_HOUR, units[i]) == Opt<std::int64_t>{hour[i]});
        CHECK(part(s, DFTU_DT_MINUTE, units[i]) ==
              Opt<std::int64_t>{minute[i]});
        CHECK(part(s, DFTU_DT_SECOND, units[i]) ==
              Opt<std::int64_t>{second[i]});
        CHECK(part(s, DFTU_DT_DAY_OF_WEEK, units[i]) == Opt<std::int64_t>{1});
        CHECK(part(s, DFTU_DT_DAY_OF_YEAR, units[i]) == Opt<std::int64_t>{318});
        CHECK(part(s, DFTU_DT_QUARTER, units[i]) == Opt<std::int64_t>{4});
        CHECK(part(s, DFTU_DT_ISO_WEEK, units[i]) == Opt<std::int64_t>{46});
        CHECK(part(s, DFTU_DT_ISO_YEAR, units[i]) == Opt<std::int64_t>{2023});
    }
}

TEST_CASE("date_part: sub-second fields, pre-epoch, leap days, ISO edges") {
    // 2023-11-14 22:13:20.123456789, the epoch, 1 ns before it, 2024-02-29
    // 12:34:56, 2021-01-03, 2024-12-30, 1900-03-01.
    const Series ns =
        ints({1'700'000'000'123'456'789, 0, -1, 1'709'210'096'000'000'000,
              1'609'632'000'000'000'000, 1'735'516'800'000'000'000,
              -2'203'891'200'000'000'000});
    CHECK(part(ns, DFTU_DT_MILLISECOND, 1)[0] ==
          std::optional<std::int64_t>(123));
    CHECK(part(ns, DFTU_DT_MICROSECOND, 1)[0] ==
          std::optional<std::int64_t>(123456));
    CHECK(part(ns, DFTU_DT_NANOSECOND, 1)[0] ==
          std::optional<std::int64_t>(789));
    CHECK(part(ns, DFTU_DT_YEAR, 1) ==
          Opt<std::int64_t>{2023, 1970, 1969, 2024, 2021, 2024, 1900});
    CHECK(part(ns, DFTU_DT_MONTH, 1) ==
          Opt<std::int64_t>{11, 1, 12, 2, 1, 12, 3});
    CHECK(part(ns, DFTU_DT_DAY, 1) ==
          Opt<std::int64_t>{14, 1, 31, 29, 3, 30, 1});
    CHECK(part(ns, DFTU_DT_HOUR, 1)[2] == std::optional<std::int64_t>(23));
    CHECK(part(ns, DFTU_DT_NANOSECOND, 1)[2] ==
          std::optional<std::int64_t>(999));
    CHECK(part(ns, DFTU_DT_DAY_OF_YEAR, 1) ==
          Opt<std::int64_t>{318, 1, 365, 60, 3, 365, 60});
    CHECK(part(ns, DFTU_DT_DAY_OF_WEEK, 1) ==
          Opt<std::int64_t>{1, 3, 2, 3, 6, 0, 3});
    CHECK(part(ns, DFTU_DT_ISO_WEEK, 1) ==
          Opt<std::int64_t>{46, 1, 1, 9, 53, 1, 9});
    CHECK(part(ns, DFTU_DT_ISO_YEAR, 1) ==
          Opt<std::int64_t>{2023, 1970, 1970, 2024, 2020, 2025, 1900});
    CHECK(part(ns, DFTU_DT_QUARTER, 1) ==
          Opt<std::int64_t>{4, 1, 4, 1, 1, 4, 1});
}

TEST_CASE("date_part and format_time: nulls, Float64 floored, other types") {
    const Series d = dbls({1'700'000'000'123'456.75, -0.5, NaN, std::nullopt});
    CHECK(part(d, DFTU_DT_MICROSECOND, 1000) ==
          Opt<std::int64_t>{123456, 999999, std::nullopt, std::nullopt});
    CHECK(fmt(d, "%F %T.%f", 1000) ==
          Opt<std::string>{"2023-11-14 22:13:20.123456",
                           "1969-12-31 23:59:59.999999", std::nullopt,
                           std::nullopt});
    const Series i = ints({1'700'000'000'123'456, std::nullopt});
    CHECK(fmt(i, "%F %T.%f", 1000) ==
          Opt<std::string>{"2023-11-14 22:13:20.123456", std::nullopt});
    const Series u = uints({1'700'000'000, U64_MAX, std::nullopt});
    CHECK(part(u, DFTU_DT_HOUR, NS_PER_S) ==
          Opt<std::int64_t>{22, std::nullopt, std::nullopt});
    const std::vector<std::int32_t> n32{86400, -1};
    const Series i32 = Series::flat(TypeId::Int32, n32.data(), 2);
    CHECK(fmt(i32, "%F %T", NS_PER_S) ==
          Opt<std::string>{"1970-01-02 00:00:00", "1969-12-31 23:59:59"});
    // v * ns_per_unit overflows 64 bits here: the instants are the int64
    // limits in microseconds.
    CHECK(fmt(ints({I64_MAX, I64_MIN}), "%F %T.%f", 1000) ==
          Opt<std::string>{"294247-01-10 04:00:54.775807",
                           "-290308-12-21 19:59:05.224192"});
}

TEST_CASE("format_time directives on fixed instants in every unit") {
    const Series ns = ints({1'700'000'000'123'456'789, -1});
    CHECK(
        fmt(ns,
            "%Y|%y|%m|%d|%H|%I|%M|%S|%f|%j|%a|%A|%b|%B|%p|%F|%T|%s|%z|%Z|%%",
            1) ==
        Opt<std::string>{
            "2023|23|11|14|22|10|13|20|123456|318|Tue|Tuesday|Nov|November|PM|"
            "2023-11-14|22:13:20|1700000000|+0000|UTC|%",
            "1969|69|12|31|23|11|59|59|999999|365|Wed|Wednesday|Dec|December|"
            "PM|1969-12-31|23:59:59|-1|+0000|UTC|%"});
    CHECK(fmt(ints({28'333'333}), "%F %T", NS_PER_MIN) ==
          Opt<std::string>{"2023-11-14 22:13:00"});
    CHECK(fmt(ints({472'222}), "%F %T", NS_PER_H) ==
          Opt<std::string>{"2023-11-14 22:00:00"});
    CHECK(fmt(ints({19'675}), "%F %T %s", NS_PER_D) ==
          Opt<std::string>{"2023-11-14 00:00:00 1699920000"});
    CHECK(fmt(ints({1'700'000'000'123}), "%T.%f", NS_PER_MS) ==
          Opt<std::string>{"22:13:20.123000"});
    CHECK(fmt(ints({-62'135'596'800}), "%Y-%m-%d", NS_PER_S) ==
          Opt<std::string>{"0001-01-01"});
    CHECK(fmt(ints({-62'135'596'801}), "%Y-%m-%d", NS_PER_S) ==
          Opt<std::string>{"0000-12-31"});
    CHECK(fmt(ints({-62'167'219'200 - 86400}), "%Y", NS_PER_S) ==
          Opt<std::string>{"-0001"});
    CHECK(fmt(ints({0}), "%I:%M %p", NS_PER_S) == Opt<std::string>{"12:00 AM"});
    CHECK(fmt(ints({43'200}), "%I:%M %p", NS_PER_S) ==
          Opt<std::string>{"12:00 PM"});
}

TEST_CASE("date_part and format_time: types, CSE and refusals") {
    const Series i = ints({1'700'000'000'123'456});
    const Series s = strs({"x"});
    for (const Expr& e : {df::expr_date_part(c0(), DFTU_DT_HOUR, 1000),
                          df::expr_format_time(c0(), "%F", 1000)}) {
        CHECK_THROWS_AS(run(e, {&s}), std::invalid_argument);
    }
    CHECK(df::infer_type(df::expr_date_part(c0(), DFTU_DT_HOUR, 1000),
                         {i.data_type()})
              .id == TypeId::Int64);
    CHECK(
        df::infer_type(df::expr_format_time(c0(), "%F", 1000), {i.data_type()})
            .id == TypeId::String);
    CHECK_THROWS_AS(df::expr_date_part(c0(), 99, 1000), std::invalid_argument);
    CHECK_THROWS_AS(df::expr_date_part(c0(), DFTU_DT_IS_LEAP_YEAR, 1000),
                    std::invalid_argument);
    CHECK_THROWS_AS(df::expr_date_part(c0(), DFTU_DT_HOUR, 0),
                    std::invalid_argument);
    CHECK_THROWS_AS(df::expr_format_time(c0(), "%Q", 1000),
                    std::invalid_argument);
    CHECK_THROWS_AS(df::expr_format_time(c0(), "%Y%", 1000),
                    std::invalid_argument);
    CHECK_THROWS_AS(df::expr_format_time(c0(), "%F", -5),
                    std::invalid_argument);

    // The same field of two units and two formats must not share a slot.
    const Expr both =
        df::expr_concat({df::expr_format_time(c0(), "%H", 1000),
                         df::expr_format_time(c0(), "%M", 1000),
                         df::expr_format_time(c0(), "%H", NS_PER_S)});
    CHECK(as_strs(run(both, {&i})) ==
          Opt<std::string>{"22"
                           "13" +
                           *fmt(i, "%H", NS_PER_S)[0]});
    const Expr sum = df::expr_arith(
        ArithOp::Add, df::expr_date_part(c0(), DFTU_DT_YEAR, 1000),
        df::expr_date_part(c0(), DFTU_DT_YEAR, NS_PER_S));
    CHECK(as_ints(run(sum, {&i})) ==
          Opt<std::int64_t>{*part(i, DFTU_DT_YEAR, 1000)[0] +
                            *part(i, DFTU_DT_YEAR, NS_PER_S)[0]});
}

TEST_CASE("date_part and format_time C ABI builders") {
    const Series i = ints({1'700'000'000'123'456, std::nullopt});
    dftu_expr* col = dftu_expr_col(0);
    dftu_expr* hour = dftu_expr_date_part(col, DFTU_DT_HOUR, 1000);
    dftu_expr* text = dftu_expr_format_time(col, "%F %T.%f", 8, 1000);
    REQUIRE(hour);
    REQUIRE(text);
    const dftu_series* raw[1] = {i.handle()};
    CHECK(as_ints(Series{dftu_expr_eval(hour, raw, 1)}) ==
          Opt<std::int64_t>{22, std::nullopt});
    CHECK(as_strs(Series{dftu_expr_eval(text, raw, 1)}) ==
          Opt<std::string>{"2023-11-14 22:13:20.123456", std::nullopt});

    CHECK(dftu_expr_date_part(nullptr, DFTU_DT_HOUR, 1000) == nullptr);
    CHECK(dftu_expr_date_part(col, 99, 1000) == nullptr);
    CHECK(dftu_expr_date_part(col, DFTU_DT_HOUR, 0) == nullptr);
    CHECK(dftu_expr_format_time(nullptr, "%F", 2, 1000) == nullptr);
    CHECK(dftu_expr_format_time(col, "%Q", 2, 1000) == nullptr);
    CHECK(dftu_expr_format_time(col, "%F%", 3, 1000) == nullptr);
    CHECK(dftu_expr_format_time(col, "%F", 2, 0) == nullptr);
    CHECK(dftu_expr_format_time(col, nullptr, 2, 1000) == nullptr);
    for (dftu_expr* e : {text, hour, col}) dftu_expr_free(e);
}
