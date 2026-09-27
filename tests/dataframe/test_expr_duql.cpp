// The duql-semantics Expr nodes: every unknown result is a null cell, and
// numbers compare exactly across integer and double columns.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
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
    CHECK(as_bools(run(df::expr_str_pattern(c0(), *iregex), {&hard})) ==
          Opt<bool>{std::nullopt, true});

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
