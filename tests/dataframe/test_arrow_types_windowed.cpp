#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/config.h>
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

// Windowed/statistical kernel coverage (rolling, ewm, cut/qcut, moments,
// quantile) for the Arrow-only logical types, mirroring
// test_arrow_types_matrix.cpp's default-less is_audited() switch.
#ifdef DFTRACER_UTILS_ENABLE_ARROW
// clang-format off
#include <nanoarrow/nanoarrow.h>
#include <dftracer/utils/dataframe/arrow.h>
#include <dftracer/utils/dataframe/scalar.h>
// clang-format on

#include "builders_arrow_types.h"

using namespace dftracer::utils::dataframe;
using namespace dftracer::utils::dataframe::test_types;

namespace {

enum class Support {
    Computes,
    Refuses,
};

constexpr Support C = Support::Computes;
constexpr Support R = Support::Refuses;

struct TypeRow {
    TypeId id;
    Series (*build)();
    Support support;
};

Series build_float16() { return make_float16({3.0f, 1.0f, 2.0f, 1.0f}); }
Series build_decimal128() { return make_decimal128({300, 100, 200, 100}); }
Series build_decimal256() { return make_decimal256({300, 100, 200, 100}); }
Series build_fixed_size_binary() {
    return make_fixed_size_binary({"cccc", "aaaa", "bbbb", "aaaa"}, 4);
}
Series build_large_string() { return make_large_utf8({"c", "a", "b", "a"}); }
Series build_large_binary() { return make_large_binary({"c", "a", "b", "a"}); }
Series build_large_list() { return make_large_list_i64({{3}, {1}, {2}, {1}}); }
Series build_fixed_size_list() {
    return make_fixed_size_list_i64({{3, 3}, {1, 1}, {2, 2}, {1, 1}});
}
Series build_map() {
    return make_map_string_i64(
        {{{"c", 3}}, {{"a", 1}}, {{"b", 2}}, {{"a", 1}}});
}

// Decimal128/256 use scale 0, so their stored values are {300,100,200,100},
// not {3,1,2,1}: every expectation below is scaled by this factor.
constexpr double DECIMAL_SCALE = 100.0;

const TypeRow TYPE_ROWS[] = {
    {TypeId::Float16, build_float16, C},
    {TypeId::Decimal128, build_decimal128, C},
    {TypeId::Decimal256, build_decimal256, C},
    {TypeId::FixedSizeBinary, build_fixed_size_binary, R},
    {TypeId::LargeString, build_large_string, R},
    {TypeId::LargeBinary, build_large_binary, R},
    {TypeId::LargeList, build_large_list, R},
    {TypeId::FixedSizeList, build_fixed_size_list, R},
    {TypeId::Map, build_map, R},
};

constexpr bool is_audited(TypeId t) {
    switch (t) {
        case TypeId::Float16:
        case TypeId::Decimal128:
        case TypeId::Decimal256:
        case TypeId::FixedSizeBinary:
        case TypeId::LargeString:
        case TypeId::LargeBinary:
        case TypeId::LargeList:
        case TypeId::FixedSizeList:
        case TypeId::Map:
            return true;
        case TypeId::Unknown:
        case TypeId::Bool:
        case TypeId::Int8:
        case TypeId::Int16:
        case TypeId::Int32:
        case TypeId::Int64:
        case TypeId::Uint8:
        case TypeId::Uint16:
        case TypeId::Uint32:
        case TypeId::Uint64:
        case TypeId::Float32:
        case TypeId::Float64:
        case TypeId::String:
        case TypeId::Binary:
        case TypeId::List:
        case TypeId::Struct:
        case TypeId::Date32:
        case TypeId::Date64:
        case TypeId::Time32:
        case TypeId::Time64:
        case TypeId::Timestamp:
        case TypeId::Duration:
            return false;
    }
    return false;
}

double scale(TypeId t) {
    return (t == TypeId::Decimal128 || t == TypeId::Decimal256) ? DECIMAL_SCALE
                                                                : 1.0;
}

}  // namespace

TEST_SUITE("dataframe_arrow_types_windowed") {
    TEST_CASE("every audited TypeId has a matrix row") {
        for (std::int32_t code = 0;
             code <= static_cast<std::int32_t>(TypeId::Map); ++code) {
            const TypeId t = static_cast<TypeId>(code);
            if (!is_audited(t)) continue;
            bool found = false;
            for (const TypeRow& row : TYPE_ROWS)
                if (row.id == t) found = true;
            INFO("no TYPE_ROWS entry for ", type_name(t));
            CHECK(found);
        }
    }

    TEST_CASE("variance, stddev, skewness, kurtosis") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            if (row.support == Support::Refuses) {
                CHECK(s.variance() == 0.0);
                CHECK(s.stddev() == 0.0);
                CHECK(s.skewness() == 0.0);
                CHECK(s.kurtosis() == 0.0);
                continue;
            }
            const double k = scale(row.id);
            CHECK(s.variance(true) == doctest::Approx(2.75 * k * k / 3.0));
            CHECK(s.stddev(true) ==
                  doctest::Approx(std::sqrt(2.75 * k * k / 3.0)));
            CHECK(s.skewness() == doctest::Approx(0.4933930822));
            CHECK(s.kurtosis() == doctest::Approx(-1.3719008264));
        }
    }

    TEST_CASE("quantile and median") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            if (row.support == Support::Refuses) {
                CHECK(std::isnan(s.quantile(0.5)));
                CHECK(std::isnan(s.median()));
                continue;
            }
            const double k = scale(row.id);
            CHECK(s.median() == doctest::Approx(1.5 * k));
            CHECK(s.quantile(0.25) == doctest::Approx(1.0 * k));
        }
    }

    TEST_CASE("rolling sum, mean, min, max") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            Series sum = s.rolling(RollingOp::Sum, 2);
            Series mean = s.rolling(RollingOp::Mean, 2);
            Series lo = s.rolling(RollingOp::Min, 2);
            Series hi = s.rolling(RollingOp::Max, 2);
            if (row.support == Support::Refuses) {
                CHECK_FALSE(sum.valid());
                CHECK_FALSE(mean.valid());
                CHECK_FALSE(lo.valid());
                CHECK_FALSE(hi.valid());
                continue;
            }
            const double k = scale(row.id);
            REQUIRE(sum.valid());
            REQUIRE(sum.length() == 4);
            CHECK(sum.is_null(0));
            const double* sd = sum.data<double>();
            REQUIRE(sd != nullptr);
            CHECK(sd[1] == doctest::Approx(4.0 * k));
            CHECK(sd[2] == doctest::Approx(3.0 * k));
            CHECK(sd[3] == doctest::Approx(3.0 * k));
            const double* md = mean.data<double>();
            REQUIRE(md != nullptr);
            CHECK(md[1] == doctest::Approx(2.0 * k));
            CHECK(md[2] == doctest::Approx(1.5 * k));
            CHECK(md[3] == doctest::Approx(1.5 * k));
            const double* lod = lo.data<double>();
            REQUIRE(lod != nullptr);
            CHECK(lod[1] == doctest::Approx(1.0 * k));
            CHECK(lod[2] == doctest::Approx(1.0 * k));
            CHECK(lod[3] == doctest::Approx(1.0 * k));
            const double* hid = hi.data<double>();
            REQUIRE(hid != nullptr);
            CHECK(hid[1] == doctest::Approx(3.0 * k));
            CHECK(hid[2] == doctest::Approx(2.0 * k));
            CHECK(hid[3] == doctest::Approx(2.0 * k));
        }
    }

    TEST_CASE("rolling_var, rolling_std, rolling_median, rolling_quantile") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            Series var = s.rolling_var(2);
            Series std_ = s.rolling_std(2);
            Series med = s.rolling_median(2);
            Series q = s.rolling_quantile(2, 0.25);
            if (row.support == Support::Refuses) {
                CHECK_FALSE(var.valid());
                CHECK_FALSE(std_.valid());
                CHECK_FALSE(med.valid());
                CHECK_FALSE(q.valid());
                continue;
            }
            const double k = scale(row.id);
            REQUIRE(var.valid());
            const double* vd = var.data<double>();
            REQUIRE(vd != nullptr);
            CHECK(vd[1] == doctest::Approx(2.0 * k * k));
            CHECK(vd[2] == doctest::Approx(0.5 * k * k));
            CHECK(vd[3] == doctest::Approx(0.5 * k * k));
            REQUIRE(std_.valid());
            const double* sdv = std_.data<double>();
            REQUIRE(sdv != nullptr);
            CHECK(sdv[1] == doctest::Approx(std::sqrt(2.0) * k));
            CHECK(sdv[2] == doctest::Approx(std::sqrt(0.5) * k));
            CHECK(sdv[3] == doctest::Approx(std::sqrt(0.5) * k));
            REQUIRE(med.valid());
            const double* medd = med.data<double>();
            REQUIRE(medd != nullptr);
            CHECK(medd[1] == doctest::Approx(2.0 * k));
            CHECK(medd[2] == doctest::Approx(1.5 * k));
            CHECK(medd[3] == doctest::Approx(1.5 * k));
            REQUIRE(q.valid());
            const double* qd = q.data<double>();
            REQUIRE(qd != nullptr);
            CHECK(qd[1] == doctest::Approx(1.5 * k));
            CHECK(qd[2] == doctest::Approx(1.25 * k));
            CHECK(qd[3] == doctest::Approx(1.25 * k));
        }
    }

    TEST_CASE("ewm_mean and ewm_std") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            Series mean = s.ewm_mean(0.5);
            Series std_ = s.ewm_std(0.5);
            if (row.support == Support::Refuses) {
                CHECK_FALSE(mean.valid());
                CHECK_FALSE(std_.valid());
                continue;
            }
            const double k = scale(row.id);
            REQUIRE(mean.valid());
            const double* md = mean.data<double>();
            REQUIRE(md != nullptr);
            // pandas adjust=True over 3, 1, 2, 1.
            CHECK(md[0] == doctest::Approx(3.0 * k));
            CHECK(md[1] == doctest::Approx(5.0 / 3.0 * k));
            CHECK(md[2] == doctest::Approx(13.0 / 7.0 * k));
            CHECK(md[3] == doctest::Approx(1.4 * k));
            REQUIRE(std_.valid());
            CHECK_FALSE(std_.is_null(1));
            const double* sdd = std_.data<double>();
            REQUIRE(sdd != nullptr);
            CHECK(sdd[1] == doctest::Approx(std::sqrt(2.0) * k));
            CHECK(sdd[2] == doctest::Approx(std::sqrt(5.0 / 7.0) * k));
            CHECK(sdd[3] == doctest::Approx(std::sqrt(0.6) * k));
        }
    }

    TEST_CASE("cut and qcut") {
        for (const TypeRow& row : TYPE_ROWS) {
            INFO(std::string(type_name(row.id)));
            Series s = row.build();
            REQUIRE(s.valid());
            const double k = scale(row.id);
            std::vector<double> edge_vals = {1.5 * k};
            Series edges = Series::flat_f64(edge_vals.data(), 1);
            Series binned = s.cut(edges);
            Series q = s.qcut(2);
            if (row.support == Support::Refuses) {
                CHECK_FALSE(binned.valid());
                CHECK_FALSE(q.valid());
                continue;
            }
            REQUIRE(binned.valid());
            REQUIRE(binned.length() == 4);
            const std::int32_t* bd = binned.data<std::int32_t>();
            REQUIRE(bd != nullptr);
            CHECK(bd[0] == 1);
            CHECK(bd[1] == 0);
            CHECK(bd[2] == 1);
            CHECK(bd[3] == 0);
            REQUIRE(q.valid());
            REQUIRE(q.length() == 4);
            const std::int32_t* qd = q.data<std::int32_t>();
            REQUIRE(qd != nullptr);
            CHECK(qd[0] == 1);
            CHECK(qd[1] == 0);
            CHECK(qd[2] == 1);
            CHECK(qd[3] == 0);
        }
    }
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW
