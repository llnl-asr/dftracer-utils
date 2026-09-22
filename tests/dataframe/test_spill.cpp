#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/internal/spill.h>
#include <dftracer/utils/dataframe/series.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using dftracer::utils::dataframe::Morsel;
using dftracer::utils::dataframe::Series;
namespace spill = dftracer::utils::dataframe::spill;

namespace {
std::optional<Morsel> run(
    dftracer::utils::coro::CoroTask<std::optional<Morsel>> t) {
    return dftracer::utils::default_runtime().submit(std::move(t)).get();
}
}  // namespace

TEST_SUITE("spill") {
    TEST_CASE("round-trip mixed-type morsels through a run file") {
        std::vector<std::int64_t> a{1, 2, 3, 4};
        std::uint8_t bm = 0b1011;  // row 2 null
        std::vector<double> b{1.5, 2.5, 3.5, 4.5};
        std::vector<std::string> s{"foo", "", "bar", "baz"};

        std::vector<Series> cols;
        cols.push_back(Series::flat_i64(a.data(), 4, &bm));
        cols.push_back(Series::flat_f64(b.data(), 4));
        cols.push_back(Series::strings(s));

        spill::Dir dir;
        const std::string path = dir.run_path(0);
        {
            spill::Writer w(path);
            w.write(cols, 4);
            w.write(cols, 4);  // two morsels in one run
            w.close();
        }

        spill::Reader r(path);
        int morsels = 0;
        while (auto m = run(r.next(0))) {
            ++morsels;
            REQUIRE(m->rows == 4);
            REQUIRE(m->columns.size() == 3);
            const std::int64_t* ap = m->columns[0].data<std::int64_t>();
            CHECK(ap[0] == 1);
            CHECK(ap[3] == 4);
            CHECK(m->columns[0].is_null(2));
            CHECK_FALSE(m->columns[0].is_null(0));
            const double* bp = m->columns[1].data<double>();
            CHECK(bp[2] == doctest::Approx(3.5));
            CHECK(m->columns[2].string_at(0) == "foo");
            CHECK(m->columns[2].string_at(1) == "");
            CHECK(m->columns[2].string_at(2) == "bar");
        }
        CHECK(morsels == 2);
    }

    TEST_CASE("empty run reads back as no morsels") {
        spill::Dir dir;
        const std::string path = dir.run_path(1);
        {
            spill::Writer w(path);
            w.close();
        }
        spill::Reader r(path);
        CHECK_FALSE(run(r.next(0)).has_value());
    }
}
