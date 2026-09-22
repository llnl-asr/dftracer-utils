#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/kernels/kernels.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dataframe/series.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

using dftracer::utils::dataframe::CmpOp;
using dftracer::utils::dataframe::dictionary_encode;
using dftracer::utils::dataframe::Encoding;
using dftracer::utils::dataframe::eval;
using dftracer::utils::dataframe::eval_many;
using dftracer::utils::dataframe::Expr;
using dftracer::utils::dataframe::expr_cmp;
using dftracer::utils::dataframe::expr_col;
using dftracer::utils::dataframe::Scalar;
using dftracer::utils::dataframe::Series;

namespace {

bool mask_bit(const Series& mask, int i) {
    const std::uint8_t* b = mask.data<std::uint8_t>();
    return ((b[i >> 3] >> (i & 7)) & 1) != 0;
}

}  // namespace

TEST_SUITE("expr string comparisons") {
    static_assert(sizeof(dftu_scalar) == 16);
    static_assert(alignof(dftu_scalar) == 8);

    TEST_CASE(
        "col == string literal masks matching rows, != is the complement") {
        Series cat = Series::strings({"POSIX", "STDIO", "POSIX", "MPIIO"});
        std::vector<const Series*> in{&cat};

        Series eq_mask =
            eval(expr_cmp(CmpOp::Eq, expr_col(0),
                          dftracer::utils::dataframe::str("POSIX")),
                 in);
        REQUIRE(eq_mask.valid());
        CHECK(mask_bit(eq_mask, 0));
        CHECK_FALSE(mask_bit(eq_mask, 1));
        CHECK(mask_bit(eq_mask, 2));
        CHECK_FALSE(mask_bit(eq_mask, 3));

        Series ne_mask =
            eval(expr_cmp(CmpOp::Ne, expr_col(0),
                          dftracer::utils::dataframe::str("POSIX")),
                 in);
        REQUIRE(ne_mask.valid());
        for (int i = 0; i < 4; ++i)
            CHECK(mask_bit(ne_mask, i) == !mask_bit(eq_mask, i));
    }

    TEST_CASE(
        "expr_cmp copies a STR rhs; mutating the source buffer afterward "
        "does not change the result") {
        Series cat = Series::strings({"POSIX", "STDIO", "POSIX"});
        std::vector<const Series*> in{&cat};

        std::string needle = "POSIX";
        Expr e = expr_cmp(CmpOp::Eq, expr_col(0),
                          dftracer::utils::dataframe::str(needle));

        needle.assign(needle.size(), 'X');  // clobber the original buffer
        needle.shrink_to_fit();

        Series mask = eval(e, in);
        REQUIRE(mask.valid());
        CHECK(mask_bit(mask, 0));
        CHECK_FALSE(mask_bit(mask, 1));
        CHECK(mask_bit(mask, 2));
    }

    TEST_CASE(
        "expr_cmp copies a STR rhs; destroying the source string "
        "afterward does not change the result") {
        Series cat = Series::strings({"POSIX", "STDIO", "POSIX"});
        std::vector<const Series*> in{&cat};

        Expr e;
        {
            std::string needle = "POSIX";
            e = expr_cmp(CmpOp::Eq, expr_col(0),
                         dftracer::utils::dataframe::str(needle));
        }  // needle destroyed here

        Series mask = eval(e, in);
        REQUIRE(mask.valid());
        CHECK(mask_bit(mask, 0));
        CHECK_FALSE(mask_bit(mask, 1));
        CHECK(mask_bit(mask, 2));
    }

    TEST_CASE("dictionary-encoded column gives the same answer as flat") {
        Series flat = Series::strings({"read", "write", "read", "open"});
        Series dict = dictionary_encode(flat);
        REQUIRE(dict.encoding() == Encoding::Dictionary);

        std::vector<const Series*> flat_in{&flat};
        std::vector<const Series*> dict_in{&dict};

        Series flat_mask =
            eval(expr_cmp(CmpOp::Eq, expr_col(0),
                          dftracer::utils::dataframe::str("read")),
                 flat_in);
        Series dict_mask =
            eval(expr_cmp(CmpOp::Eq, expr_col(0),
                          dftracer::utils::dataframe::str("read")),
                 dict_in);

        REQUIRE(flat_mask.valid());
        REQUIRE(dict_mask.valid());
        for (int i = 0; i < 4; ++i)
            CHECK(mask_bit(flat_mask, i) == mask_bit(dict_mask, i));
        CHECK(mask_bit(flat_mask, 0));
        CHECK_FALSE(mask_bit(flat_mask, 1));
        CHECK(mask_bit(flat_mask, 2));
        CHECK_FALSE(mask_bit(flat_mask, 3));
    }

    TEST_CASE("ordered comparison on a string scalar is refused at the C ABI") {
        Series cat = Series::strings({"POSIX", "STDIO"});
        dftu_scalar rhs = dftracer::utils::dataframe::str("POSIX");

        CHECK(dftu_series_compare(cat.handle(), DFTU_CMP_GT, rhs) == nullptr);
        CHECK(dftu_series_compare(cat.handle(), DFTU_CMP_GE, rhs) == nullptr);
        CHECK(dftu_series_compare(cat.handle(), DFTU_CMP_LT, rhs) == nullptr);
        CHECK(dftu_series_compare(cat.handle(), DFTU_CMP_LE, rhs) == nullptr);

        dftu_series* eq = dftu_series_compare(cat.handle(), DFTU_CMP_EQ, rhs);
        REQUIRE(eq != nullptr);
        dftu_series_free(eq);
    }

    TEST_CASE(
        "scalar_value<T> on a STR tag returns T{}; Scalar::str() is text "
        "only for STR") {
        dftu_scalar raw = dftracer::utils::dataframe::str("POSIX");
        CHECK(dftracer::utils::dataframe::scalar_value<std::int64_t>(raw) == 0);
        CHECK(dftracer::utils::dataframe::scalar_value<double>(raw) == 0.0);

        Scalar str_scalar(raw);
        CHECK(str_scalar.str() == "POSIX");

        Scalar num_scalar(
            dftracer::utils::dataframe::to_scalar<std::int64_t>(42));
        CHECK(num_scalar.str().empty());
    }

    TEST_CASE(
        "CSE keys on string content: two different literals in one "
        "eval_many stay distinct") {
        Series cat = Series::strings({"POSIX", "STDIO", "POSIX"});
        std::vector<const Series*> in{&cat};

        Expr posix_eq = expr_cmp(CmpOp::Eq, expr_col(0),
                                 dftracer::utils::dataframe::str("POSIX"));
        Expr stdio_eq = expr_cmp(CmpOp::Eq, expr_col(0),
                                 dftracer::utils::dataframe::str("STDIO"));

        std::vector<Series> outs = eval_many({posix_eq, stdio_eq}, in);
        REQUIRE(outs.size() == 2);
        REQUIRE(outs[0].valid());
        REQUIRE(outs[1].valid());

        CHECK(mask_bit(outs[0], 0));
        CHECK_FALSE(mask_bit(outs[0], 1));
        CHECK(mask_bit(outs[0], 2));

        CHECK_FALSE(mask_bit(outs[1], 0));
        CHECK(mask_bit(outs[1], 1));
        CHECK_FALSE(mask_bit(outs[1], 2));

        bool any_diff = false;
        for (int i = 0; i < 3; ++i) {
            if (mask_bit(outs[0], i) != mask_bit(outs[1], i)) any_diff = true;
        }
        CHECK(any_diff);
    }

    // DFTU_SCALAR_I64/_U64/_F64/_STR are C-only (guarded by #ifndef
    // __cplusplus in abi.h), so they are exercised from a C TU instead:
    // see test_scalar_macros_c.c.
}
