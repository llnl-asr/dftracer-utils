// The expression forms of the eager Series.str methods: each gives the eager
// result (values, nulls, type), infer_type agrees with eval, index / rindex
// fail the plan run on a miss, and a bool cast then capitalize spells
// True / False.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using dftracer::utils::dataframe::DataType;
using dftracer::utils::dataframe::eval;
using dftracer::utils::dataframe::Expr;
using dftracer::utils::dataframe::expr_cast;
using dftracer::utils::dataframe::expr_col;
using dftracer::utils::dataframe::expr_list_get;
using dftracer::utils::dataframe::expr_str_fn;
using dftracer::utils::dataframe::expr_str_map;
using dftracer::utils::dataframe::infer_type;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::StrFn;
using dftracer::utils::dataframe::StrMapOp;
using dftracer::utils::dataframe::TypeId;

namespace {

Series sample() {
    const std::vector<std::string_view> rows{
        "hello world", "Foo-Bar,baz", "",    "  x1 ",
        "ABC",         "a,b,c",       "-42", "null row"};
    const std::uint8_t validity = 0b11111011;  // row 2 is null
    return Series::strings(rows, &validity);
}

// Equal rows, nulls and type.
void same(const Series& got, const Series& want) {
    REQUIRE(got.valid());
    REQUIRE(want.valid());
    REQUIRE(got.type() == want.type());
    REQUIRE(got.length() == want.length());
    for (std::int64_t i = 0; i < want.length(); ++i) {
        REQUIRE(got.is_null(i) == want.is_null(i));
        if (want.is_null(i)) continue;
        if (want.type() == TypeId::String)
            REQUIRE(got.string_at(i) == want.string_at(i));
        else if (want.type() == TypeId::Int64)
            REQUIRE(got.data<std::int64_t>()[i] ==
                    want.data<std::int64_t>()[i]);
        else if (want.type() == TypeId::Bool)
            REQUIRE((((got.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1)) ==
                    (((want.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1)));
    }
}

Series run(const Expr& e, const Series& s) {
    std::vector<const Series*> in{&s};
    Series out = eval(e, in);
    DataType dt = infer_type(e, {s.data_type()});
    REQUIRE(out.valid());
    CHECK(dt.id == out.type());
    return out;
}

Expr fn(StrFn f, std::string_view t = {}, std::int64_t i0 = 0,
        std::string_view t2 = {}, std::int64_t i1 = 0) {
    return expr_str_fn(f, expr_col(0), nullptr, t, t2, i0, i1);
}

}  // namespace

TEST_SUITE("expression string methods") {
    TEST_CASE("character classes equal the eager kernels") {
        Series s = sample();
        for (int cls = 0; cls <= 8; ++cls)
            same(run(fn(static_cast<StrFn>(cls)), s),
                 Series{dftu_series_str_is(s.handle(), cls)});
    }

    TEST_CASE("case maps") {
        Series s = sample();
        same(run(expr_str_map(StrMapOp::Capitalize, expr_col(0)), s),
             Series{dftu_series_str_case(s.handle(), DFTU_STR_CAPITALIZE)});
        same(run(expr_str_map(StrMapOp::Title, expr_col(0)), s),
             Series{dftu_series_str_case(s.handle(), DFTU_STR_TITLE_CASE)});
        same(run(expr_str_map(StrMapOp::Swapcase, expr_col(0)), s),
             Series{dftu_series_str_case(s.handle(), DFTU_STR_SWAPCASE)});
    }

    TEST_CASE("argument maps equal the eager kernels") {
        Series s = sample();
        same(run(fn(StrFn::PadStart, "*", 14), s),
             Series{dftu_series_str_pad_start(s.handle(), 14, '*')});
        same(run(fn(StrFn::PadEnd, "*", 14), s),
             Series{dftu_series_str_pad_end(s.handle(), 14, '*')});
        same(run(fn(StrFn::Center, ".", 15), s),
             Series{dftu_series_str_center(s.handle(), 15, '.')});
        same(run(fn(StrFn::Zfill, {}, 8), s),
             Series{dftu_series_str_zfill(s.handle(), 8)});
        same(run(fn(StrFn::RemovePrefix, "he"), s),
             Series{dftu_series_str_remove_prefix(s.handle(), "he", 2)});
        same(run(fn(StrFn::RemoveSuffix, "ld"), s),
             Series{dftu_series_str_remove_suffix(s.handle(), "ld", 2)});
        same(run(fn(StrFn::Repeat, {}, 3), s),
             Series{dftu_series_str_repeat(s.handle(), 3)});
        same(run(fn(StrFn::Rfind, "o"), s),
             Series{dftu_series_str_rfind(s.handle(), "o", 1)});
    }

    TEST_CASE("slice_replace composes slice and cat as the eager form does") {
        Series s = sample();
        Series out =
            run(fn(StrFn::SliceReplace, "##", 2, {}, std::int64_t{4}), s);
        REQUIRE(out.type() == TypeId::String);
        CHECK(out.string_at(0) == "he##o world");
        CHECK(out.is_null(2));
        CHECK(out.string_at(4) == "AB##");  // the tail past 4 is empty
        // No stop: the rest of the row is replaced.
        Series open = run(fn(StrFn::SliceReplace, "!", 1, {},
                             std::numeric_limits<std::int64_t>::min()),
                          s);
        CHECK(open.string_at(0) == "h!");
    }

    TEST_CASE("splitting and extraction") {
        Series s = sample();
        Series parts = run(fn(StrFn::Split, ","), s);
        CHECK(parts.type() == TypeId::List);
        Series eager{dftu_series_str_split(s.handle(), ",", 1)};
        REQUIRE(eager.valid());
        CHECK(parts.length() == eager.length());
        // The pieces by position, through the list element expression.
        Series second = run(expr_list_get(fn(StrFn::Split, ","), 1), s);
        CHECK(second.string_at(1) == "baz");
        Series joined = run(expr_str_fn(StrFn::Join, fn(StrFn::Split, ","),
                                        nullptr, "+", {}, 0, 0),
                            s);
        CHECK(joined.string_at(5) == "a+b+c");
        CHECK(joined.is_null(2));
        Series head = run(expr_list_get(fn(StrFn::Partition, ","), 0), s);
        CHECK(head.string_at(5) == "a");
        Series tail = run(expr_list_get(fn(StrFn::RPartition, ","), 2), s);
        CHECK(tail.string_at(5) == "c");
        Series digits = run(fn(StrFn::Extract, "([0-9]+)", 1), s);
        CHECK(digits.string_at(3) == "1");
        CHECK(digits.string_at(6) == "42");
        Series all = run(expr_list_get(fn(StrFn::Findall, "[a-z]+"), -1), s);
        CHECK(all.string_at(5) == "c");
    }

    TEST_CASE("get reads a list element or a string byte") {
        Series s = sample();
        Series byte = run(fn(StrFn::Get, {}, 1), s);
        CHECK(byte.string_at(0) == "e");
        CHECK(byte.is_null(2));  // a null row
        CHECK(byte.is_null(7) == false);
        Series past = run(fn(StrFn::Get, {}, 50), s);
        CHECK(past.is_null(0));
        CHECK_THROWS_AS(run(fn(StrFn::Get, {}, -1), s), std::invalid_argument);
        // On a list the same call reads the element.
        Series elem = run(expr_str_fn(StrFn::Get, fn(StrFn::Split, ","),
                                      nullptr, {}, {}, 1, 0),
                          s);
        CHECK(elem.string_at(1) == "baz");
    }

    TEST_CASE("cat joins two columns with a separator") {
        Series s = sample();
        Series t = Series::strings({"1", "2", "3", "4", "5", "6", "7", "8"});
        std::vector<const Series*> in{&s, &t};
        Expr e = expr_str_fn(StrFn::Cat, expr_col(0), nullptr, "-", {}, 0, 0);
        CHECK_THROWS_AS(eval(e, in), std::invalid_argument);  // no other column
        const Expr other = expr_col(1);
        Expr cat = expr_str_fn(StrFn::Cat, expr_col(0), &other, "-", {}, 0, 0);
        Series out = eval(cat, in);
        REQUIRE(out.valid());
        CHECK(out.string_at(0) == "hello world-1");
        CHECK(out.is_null(2));
    }

    TEST_CASE("index and rindex fail the run on a row without the substring") {
        Series s = sample();
        std::string message;
        try {
            run(fn(StrFn::Index, "o"), s);
        } catch (const std::invalid_argument& e) {
            message = e.what();
        }
        CHECK(message.find("not found in every row") != std::string::npos);
        CHECK(message.find("'o'") != std::string::npos);
        // A null row is not a miss.
        const std::uint8_t only_first = 0b01;
        const std::vector<std::string_view> pair{"foo", "x"};
        Series with_null = Series::strings(pair, &only_first);
        Series idx0 = run(fn(StrFn::Index, "o"), with_null);
        CHECK(idx0.is_null(1));
        Series ok = Series::strings({"foo", "bo"});
        Series idx = run(fn(StrFn::Index, "o"), ok);
        CHECK(idx.data<std::int64_t>()[0] == 1);
        Series ridx = run(fn(StrFn::Rindex, "o"), ok);
        CHECK(ridx.data<std::int64_t>()[0] == 2);
    }

    TEST_CASE("a bool cast to string then capitalize spells True and False") {
        const std::uint8_t bits = 0b101;
        Series b = Series::flat(TypeId::Bool, &bits, 3);
        Series out = run(expr_str_map(StrMapOp::Capitalize,
                                      expr_cast(TypeId::String, expr_col(0))),
                         b);
        REQUIRE(out.type() == TypeId::String);
        CHECK(out.string_at(0) == "True");
        CHECK(out.string_at(1) == "False");
        CHECK(out.string_at(2) == "True");
    }

    TEST_CASE("a non-string operand is refused") {
        const std::int64_t v[] = {1, 2};
        Series n = Series::flat_i64(v, 2);
        std::vector<const Series*> in{&n};
        CHECK_THROWS_AS(eval(fn(StrFn::IsAlpha), in), std::invalid_argument);
        CHECK(dftu_expr_str_fn(99, nullptr, nullptr, "", 0, "", 0, 0, 0) ==
              nullptr);
    }
}
