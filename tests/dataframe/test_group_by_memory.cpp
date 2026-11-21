// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/int128.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/debug.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

// Four morsels and a sixteenth, so the group-by folds in lanes that each hold
// every group.
constexpr std::int64_t ROWS = 262144;
constexpr std::int64_t GROUPS = 112656;
constexpr std::int64_t AGGS = 8;
constexpr std::size_t LANES = 4;
// What a light group row keeps per aggregation, a word, and what the key table
// and the first-seen order keep per group.
constexpr std::size_t CELL_BYTES = 8;
constexpr std::size_t GROUP_BYTES = 64;
// The peak snapshot trails the true peak by at most this much.
constexpr std::size_t SNAPSHOT_SLACK = std::size_t{16} << 20;

std::size_t live() {
    memory::detail::debug_drain();
    return memory::detail::Manager::get().root().live();
}

}  // namespace

// Needs DFTRACER_UTILS_MEMORY_DEBUG=1, which tests/dataframe/CMakeLists.txt
// sets.
TEST_CASE(
    "group_by holds state in proportion to the groups and the aggregates") {
    std::mt19937_64 rng(1);
    dstd::Vector<std::int64_t> key(ROWS);
    for (auto& v : key) v = static_cast<std::int64_t>(rng() % GROUPS);
    DataFrame df;
    df.names.emplace_back("g");
    df.columns.push_back(Series::flat_i64(key.data(), ROWS));
    dstd::Vector<double> val(ROWS);
    dstd::Vector<std::uint8_t> valid((ROWS + 7) / 8);
    dstd::Vector<GroupAgg> aggs;
    for (std::int64_t c = 0; c < AGGS; ++c) {
        for (std::int64_t i = 0; i < ROWS; ++i) {
            val[i] = static_cast<double>(rng() % 1000) / 7.0;
            const bool present = rng() % 20 != 0;
            if (present)
                valid[i >> 3] |= static_cast<std::uint8_t>(1U << (i & 7));
            else
                valid[i >> 3] &= static_cast<std::uint8_t>(~(1U << (i & 7)));
        }
        const dstd::String name(dstd::fmt<"c{}">(c));
        df.names.emplace_back(dstd::StringView(name));
        df.columns.push_back(Series::flat_f64(val.data(), ROWS, valid.data()));
        aggs.push_back(
            GroupAgg{Agg::Sum, name, dstd::String(dstd::fmt<"s{}">(c))});
    }

    const std::size_t before = live();
    auto out = df.group_by("g", aggs);
    REQUIRE(out.has_value());
    const memory::detail::DebugPeak peak =
        memory::detail::debug_top_sites_at_peak(
            0, [](void*, const memory::detail::DebugSiteTotal&) noexcept {},
            nullptr);

    dstd::Vector<std::uint8_t> seen(GROUPS);
    std::int64_t distinct = 0;
    for (const std::int64_t k : key)
        if (!seen[static_cast<std::size_t>(k)]++) ++distinct;
    CHECK(out->num_rows() == distinct);

    // The snapshot sees the debug mode's own counting, so a build without it
    // reports nothing and the check below must not pass on that.
    REQUIRE(peak.total > before);
    const std::size_t held = peak.total - before;
    const auto groups = static_cast<std::size_t>(distinct);
    const std::size_t budget =
        groups * (static_cast<std::size_t>(AGGS) * CELL_BYTES * (LANES + 1) +
                  GROUP_BYTES * LANES) +
        SNAPSHOT_SLACK;
    CHECK(held <= budget);
}

TEST_CASE("group_by merges integer sums that wrap and unwrap across lanes") {
    // Chunks 0, 1 and 4 add 2^47 a row and chunks 2 and 3 subtract it, so the
    // lanes' partial sums are +2^62, +2^62, -2^62 and -2^62 and the merge
    // passes through a wrap each way. The exact total of a group is 2^58. A
    // second column adds 2^47 a row throughout, so its total does not fit.
    constexpr std::int64_t MORSEL = std::int64_t{1} << 16;
    constexpr std::int64_t N = 4 * MORSEL + MORSEL / 16;
    constexpr std::int64_t STEP = std::int64_t{1} << 47;
    dstd::Vector<std::int64_t> key(N), val(N), up(N, STEP);
    for (std::int64_t i = 0; i < N; ++i) {
        key[i] = i % 2;
        const std::int64_t chunk = i / MORSEL;
        val[i] = chunk == 2 || chunk == 3 ? -STEP : STEP;
    }
    DataFrame df;
    df.names = dstd::make_shared_strings({"g", "v", "w"});
    df.columns.push_back(Series::flat_i64(key.data(), N));
    df.columns.push_back(Series::flat_i64(val.data(), N));
    df.columns.push_back(Series::flat_i64(up.data(), N));
    const dstd::Vector<GroupAgg> aggs{
        GroupAgg{Agg::Sum, "v", "sum"}, GroupAgg{Agg::Min, "v", "lo"},
        GroupAgg{Agg::Max, "v", "hi"}, GroupAgg{Agg::Mean, "v", "mean"},
        GroupAgg{Agg::Sum, "w", "overflow"}};
    DataFrame out = dftu_utils_test::ok(df.group_by("g", aggs));
    REQUIRE(out.num_rows() == 2);
    const double rows_per_group = static_cast<double>(N / 2);
    for (std::int64_t g = 0; g < 2; ++g) {
        REQUIRE(!out.column("sum").is_null(g));
        CHECK(out.column("sum").data<std::int64_t>()[g] == std::int64_t{1}
                                                               << 58);
        CHECK(out.column("lo").data<std::int64_t>()[g] == -STEP);
        CHECK(out.column("hi").data<std::int64_t>()[g] == STEP);
        CHECK(out.column("overflow").is_null(g));
        CHECK(out.column("mean").data<double>()[g] ==
              static_cast<double>(std::int64_t{1} << 58) / rows_per_group);
    }
}

TEST_CASE("group_by keeps every value through the lane merge and the reorder") {
    // 112656 keys over four lanes: the merged groups are not in first-seen
    // order, so they are permuted, wraps and all. The reference folds the rows
    // in row order; the lanes associate a float sum differently, so floats are
    // compared to 1e-9 relative and integers exactly.
    constexpr std::int64_t N = 262144;
    constexpr std::int64_t KEYS = 112656;
    std::mt19937_64 rng(3);
    dstd::Vector<std::int64_t> key(N), iv(N);
    dstd::Vector<double> fv(N);
    dstd::Vector<std::uint8_t> valid((N + 7) / 8, 0);
    for (std::int64_t i = 0; i < N; ++i) {
        key[i] = static_cast<std::int64_t>(rng() % KEYS);
        const auto m =
            static_cast<std::int64_t>(rng() % (std::uint64_t{1} << 62));
        iv[i] = (rng() & 1) ? m : -m;
        fv[i] = static_cast<double>(rng() % 100000) / 7.0;
        if (rng() % 20)
            valid[i >> 3] |= static_cast<std::uint8_t>(1U << (i & 7));
    }
    DataFrame df;
    df.names = dstd::make_shared_strings({"g", "i", "f"});
    df.columns.push_back(Series::flat_i64(key.data(), N));
    df.columns.push_back(Series::flat_i64(iv.data(), N));
    df.columns.push_back(Series::flat_f64(fv.data(), N, valid.data()));
    const dstd::Vector<GroupAgg> aggs{
        GroupAgg{Agg::Sum, "i", "si"}, GroupAgg{Agg::Mean, "i", "mi"},
        GroupAgg{Agg::Min, "i", "lo"}, GroupAgg{Agg::Max, "i", "hi"},
        GroupAgg{Agg::Sum, "f", "sf"}, GroupAgg{Agg::Mean, "f", "mf"},
        GroupAgg{Agg::Var, "f", "vf"}, GroupAgg{Agg::Std, "f", "sdf"},
        GroupAgg{Agg::Min, "f", "lf"}, GroupAgg{Agg::Max, "f", "hf"}};
    DataFrame out = dftu_utils_test::ok(df.group_by("g", aggs));

    struct Expect {
        dstd::int128_t si = 0;
        std::int64_t lo = INT64_MAX, hi = INT64_MIN, rows = 0, n = 0;
        double sf = 0, lf = 0, hf = 0;
        dstd::Vector<double> xs;
    };
    dstd::HashMap<std::int64_t, Expect> expect;
    dstd::Vector<std::int64_t> first_seen;
    for (std::int64_t i = 0; i < N; ++i) {
        const auto [it, fresh] = expect.try_emplace(key[i]);
        if (fresh) first_seen.push_back(key[i]);
        Expect& e = it->second;
        e.si += iv[i];
        ++e.rows;
        e.lo = std::min(e.lo, iv[i]);
        e.hi = std::max(e.hi, iv[i]);
        if (!((valid[i >> 3] >> (i & 7)) & 1)) continue;
        if (e.n++ == 0) e.lf = e.hf = fv[i];
        e.sf += fv[i];
        e.lf = std::min(e.lf, fv[i]);
        e.hf = std::max(e.hf, fv[i]);
        e.xs.push_back(fv[i]);
    }
    REQUIRE(out.num_rows() == static_cast<std::int64_t>(first_seen.size()));
    auto close = [](double a, double b) {
        return std::fabs(a - b) <= 1e-9 * (1.0 + std::fabs(b));
    };
    std::int64_t wrapped = 0, bad = 0;
    for (std::int64_t g = 0; g < out.num_rows(); ++g) {
        const Expect& e = expect[first_seen[static_cast<std::size_t>(g)]];
        bool ok = out.column("g").data<std::int64_t>()[g] ==
                  first_seen[static_cast<std::size_t>(g)];
        const bool fits = e.si >= INT64_MIN && e.si <= INT64_MAX;
        wrapped += !fits;
        ok = ok && out.column("si").is_null(g) == !fits;
        if (fits)
            ok = ok && out.column("si").data<std::int64_t>()[g] ==
                           static_cast<std::int64_t>(e.si);
        ok = ok && out.column("lo").data<std::int64_t>()[g] == e.lo &&
             out.column("hi").data<std::int64_t>()[g] == e.hi &&
             close(out.column("mi").data<double>()[g],
                   static_cast<double>(e.si) / static_cast<double>(e.rows));
        if (e.n > 0) {
            const double mean = e.sf / static_cast<double>(e.n);
            ok = ok && close(out.column("sf").data<double>()[g], e.sf) &&
                 close(out.column("mf").data<double>()[g], mean) &&
                 out.column("lf").data<double>()[g] == e.lf &&
                 out.column("hf").data<double>()[g] == e.hf;
            if (e.n > 1) {
                double ss = 0;
                for (const double x : e.xs) ss += (x - mean) * (x - mean);
                const double var = ss / static_cast<double>(e.n - 1);
                ok = ok && close(out.column("vf").data<double>()[g], var) &&
                     close(out.column("sdf").data<double>()[g], std::sqrt(var));
            }
        } else {
            // No value: the sum is 0, the mean and the extremes are null.
            ok = ok && !out.column("sf").is_null(g) &&
                 out.column("sf").data<double>()[g] == 0.0 &&
                 out.column("mf").is_null(g) && out.column("lf").is_null(g) &&
                 out.column("hf").is_null(g);
        }
        bad += !ok;
    }
    CHECK(bad == 0);
    CHECK(wrapped > 1000);
}
