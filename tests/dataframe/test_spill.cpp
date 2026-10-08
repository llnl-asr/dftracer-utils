#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/internal/spill.h>
#include <dftracer/utils/dataframe/series.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
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

    TEST_CASE("compressed morsels read back exactly and take less room") {
        struct Env {
            explicit Env(const char* v) {
                ::setenv("DFTRACER_UTILS_SPILL_COMPRESS", v, 1);
            }
            ~Env() { ::unsetenv("DFTRACER_UTILS_SPILL_COMPRESS"); }
        };
        constexpr std::int64_t N = 20000;
        std::vector<std::int64_t> a(N);
        std::vector<std::string> s(N);
        std::vector<std::uint8_t> valid((N + 7) / 8, 0xFF);
        std::uint64_t x = 88172645463325252ULL;
        for (std::int64_t i = 0; i < N; ++i) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            a[i] = i < N / 2 ? i % 97 : static_cast<std::int64_t>(x);
            s[i] = "name" + std::to_string(i % 31);
            if (i % 13 == 0) valid[i / 8] &= ~(1u << (i % 8));
        }
        std::vector<Series> cols;
        cols.push_back(Series::flat_i64(a.data(), N, valid.data()));
        cols.push_back(Series::strings(s));

        auto round_trip = [&](const char* mode, std::uintmax_t* bytes) {
            Env env(mode);
            spill::Dir dir;
            const std::string path = dir.run_path(0);
            {
                spill::Writer w(path);
                w.write(cols, N);
                w.write(cols, N);
                w.close();
            }
            *bytes = fs::file_size(path);
            spill::Reader r(path);
            int morsels = 0;
            while (auto m = run(r.next(0))) {
                ++morsels;
                REQUIRE(m->rows == N);
                for (std::int64_t i = 0; i < N; ++i) {
                    REQUIRE(m->columns[0].is_null(i) == (i % 13 == 0));
                    if (i % 13 != 0)
                        REQUIRE(m->columns[0].data<std::int64_t>()[i] == a[i]);
                    REQUIRE(m->columns[1].string_at(i) == s[i]);
                }
            }
            CHECK(morsels == 2);
        };
        std::uintmax_t raw_bytes = 0, packed_bytes = 0;
        round_trip("off", &raw_bytes);
        round_trip("on", &packed_bytes);
        CHECK(packed_bytes < raw_bytes);
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

    TEST_CASE("a JSON column keeps its flag through a run file") {
        Series text = Series::strings(std::vector<std::string>{"1", "\"a\""});
        std::vector<Series> cols;
        cols.push_back(text.as_json());
        cols.push_back(Series::strings(std::vector<std::string>{"x", "y"}));
        spill::Dir dir;
        const std::string path = dir.run_path(0);
        {
            spill::Writer w(path);
            w.write(cols, 2);
            w.close();
        }
        spill::Reader r(path);
        auto m = run(r.next(0));
        REQUIRE(m);
        CHECK(m->columns[0].is_json());
        CHECK_FALSE(m->columns[1].is_json());
        CHECK(m->columns[0].string_at(1) == "\"a\"");
    }
}

TEST_SUITE("spill errors") {
    TEST_CASE("a run in a missing directory cannot be opened") {
        CHECK_THROWS(spill::Writer("/dftu_no_such_dir/run0"));
        CHECK_THROWS(spill::Reader("/dftu_no_such_dir/run0"));
        CHECK_THROWS(spill::AggRunReader("/dftu_no_such_dir/run0"));
    }

    TEST_CASE("a truncated run is an error, not the end of the run") {
        std::vector<std::int64_t> a{1, 2, 3, 4, 5, 6, 7, 8};
        std::vector<Series> cols;
        cols.push_back(Series::flat_i64(a.data(), 8));
        spill::Dir dir;
        const std::string path = dir.run_path(0);
        {
            spill::Writer w(path);
            w.write(cols, 8);
            w.write(cols, 8);
            w.close();
        }
        const std::uintmax_t full = fs::file_size(path);
        auto drain = [&] {
            spill::Reader r(path);
            while (auto m = run(r.next(0))) {
            }
        };
        CHECK_NOTHROW(drain());
        // Cut inside the second morsel's payload, then inside its header.
        fs::resize_file(path, full - 5);
        CHECK_THROWS(drain());
        fs::resize_file(path, full / 2 + 5);
        CHECK_THROWS(drain());
    }

    TEST_CASE("a truncated aggregate run is an error") {
        spill::Dir dir;
        const std::string path = dir.run_path(0);
        {
            std::ofstream os(path, std::ios::binary);
            const std::uint32_t len = 100;
            os.write(reinterpret_cast<const char*>(&len), sizeof(len));
            os.write("abcd", 4);
        }
        CHECK_THROWS(spill::AggRunReader(path));
        {
            std::ofstream os(path, std::ios::binary);
            os.write("ab", 2);
        }
        CHECK_THROWS(spill::AggRunReader(path));
        {
            std::ofstream os(path, std::ios::binary);
        }
        spill::AggRunReader empty(path);
        CHECK_FALSE(empty.valid());
    }
}
