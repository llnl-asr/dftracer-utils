#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/duql/overlap.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace df = dftracer::utils::dataframe;
using namespace dftracer::utils::duql;

namespace {

struct Rng {
    std::uint64_t x;
    std::uint64_t next(std::uint64_t m) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        return (x >> 33) % m;
    }
};

struct Table {
    std::vector<std::int64_t> key;
    std::vector<std::int64_t> start;
    std::vector<std::int64_t> duration;
    std::vector<std::uint8_t> key_ok, start_ok, duration_ok;
};

Table random_table(Rng& rng, int n, int keys, int span, int max_dur) {
    Table t;
    for (int i = 0; i < n; ++i) {
        t.key.push_back(static_cast<std::int64_t>(rng.next(keys)));
        t.start.push_back(static_cast<std::int64_t>(rng.next(span)));
        // Zero and negative durations are in the mix.
        t.duration.push_back(static_cast<std::int64_t>(rng.next(max_dur + 3)) -
                             2);
        t.key_ok.push_back(rng.next(20) != 0);
        t.start_ok.push_back(rng.next(20) != 0);
        t.duration_ok.push_back(rng.next(20) != 0);
    }
    return t;
}

df::Series column(const std::vector<std::int64_t>& v,
                  const std::vector<std::uint8_t>& ok) {
    std::vector<std::uint8_t> bits((v.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < v.size(); ++i)
        if (ok[i]) bits[i >> 3] |= static_cast<std::uint8_t>(1U << (i & 7));
    return df::Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size()),
                                bits.data());
}

OverlapColumns columns_of(const Table& t) {
    OverlapColumns c;
    c.keys.push_back(column(t.key, t.key_ok));
    c.start = column(t.start, t.start_ok);
    c.duration = column(t.duration, t.duration_ok);
    return c;
}

bool usable(const Table& t, std::size_t i) {
    return t.key_ok[i] && t.start_ok[i] && t.duration_ok[i] &&
           t.duration[i] >= 0;
}

OverlapMatches nested_loop(const Table& rows, const Table& side) {
    OverlapMatches out;
    out.offsets.push_back(0);
    for (std::size_t i = 0; i < rows.key.size(); ++i) {
        if (usable(rows, i))
            for (std::size_t r = 0; r < side.key.size(); ++r) {
                if (!usable(side, r) || side.key[r] != rows.key[i]) continue;
                const std::int64_t a = rows.start[i], d = rows.duration[i];
                const std::int64_t b = side.start[r], e = side.duration[r];
                if (b < a + d && a < b + e)
                    out.rows.push_back(static_cast<std::int64_t>(r));
            }
        out.offsets.push_back(static_cast<std::int64_t>(out.rows.size()));
    }
    return out;
}

}  // namespace

TEST_CASE("overlap equals a nested loop") {
    Rng rng{7};
    for (const auto& c : std::vector<std::vector<int>>{{200, 150, 3, 100, 20},
                                                       {300, 300, 1, 50, 5},
                                                       {100, 400, 5, 1000, 400},
                                                       {0, 10, 2, 10, 3},
                                                       {10, 0, 2, 10, 3}}) {
        const int n = c[0], m = c[1], keys = c[2], span = c[3], dur = c[4];
        CAPTURE(n);
        CAPTURE(m);
        const Table rows = random_table(rng, n, keys, span, dur);
        const Table side = random_table(rng, m, keys, span, dur);
        const OverlapMatches got =
            overlap_matches(columns_of(rows), columns_of(side));
        const OverlapMatches want = nested_loop(rows, side);
        CHECK(got.offsets == want.offsets);
        CHECK(got.rows == want.rows);
    }
}

TEST_CASE("overlap is half open and zero-length intervals sit inside") {
    Table side;
    side.key = {1, 1, 1};
    side.start = {0, 10, 5};
    side.duration = {10, 10, 0};
    side.key_ok = side.start_ok = side.duration_ok = {1, 1, 1};
    Table rows;
    rows.key = {1, 1, 1, 1};
    rows.start = {10, 9, 5, 4};
    rows.duration = {5, 2, 0, 4};
    rows.key_ok = rows.start_ok = rows.duration_ok = {1, 1, 1, 1};
    const auto m = overlap_matches(columns_of(rows), columns_of(side));
    // [10,15) meets [0,10) not and takes [10,20); [9,11) takes both; [5,5)
    // is inside [0,10) only; [4,8) takes [0,10) and the zero-length row at 5.
    const std::vector<std::int64_t> rows_want = {1, 0, 1, 0, 0, 2};
    const std::vector<std::int64_t> offsets_want = {0, 1, 3, 4, 6};
    CHECK(m.offsets == offsets_want);
    CHECK(m.rows == rows_want);
}

TEST_CASE("overlap does not scan pairs") {
    Rng rng{11};
    const int n = static_cast<int>(DFTRACER_UTILS_VALGRIND_SCALE(200000, 20));
    const Table rows = random_table(rng, n, 4, 5'000'000, 50);
    const Table side = random_table(rng, n, 4, 5'000'000, 50);
    const auto t0 = std::chrono::steady_clock::now();
    const auto m = overlap_matches(columns_of(rows), columns_of(side));
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    CHECK(m.offsets.size() == static_cast<std::size_t>(n) + 1);
    CHECK(seconds < 10.0);  // a pair scan takes minutes
}

TEST_CASE("overlap compares doubles and scaled durations") {
    const double start[] = {0.5, 3.0};
    const double dur[] = {1.0, 1.0};
    OverlapColumns side;
    side.keys.push_back(
        df::Series::flat_i64(std::vector<std::int64_t>{1, 1}.data(), 2));
    side.start = df::Series::flat_f64(start, 2);
    side.duration = df::Series::flat_f64(dur, 2);
    const std::int64_t one = 1, at = 1, len = 3000;
    OverlapColumns rows;
    rows.keys.push_back(df::Series::flat_i64(&one, 1));
    rows.start = df::Series::flat_i64(&at, 1);
    rows.duration = df::Series::flat_i64(&len, 1);
    rows.scale = 0.001;  // 3000 ns is 3 in the unit of the start
    const auto m = overlap_matches(rows, side);
    CHECK(m.rows == std::vector<std::int64_t>{0, 1});
}
