// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/int128.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;

namespace {

// Four morsels, which the lanes of a fold without disjoint groups would each
// hold every group of.
constexpr std::int64_t ROWS = 262144;
constexpr std::int64_t GROUPS = 112656;
constexpr std::int64_t MANY = 15;
// A word is what one group keeps per aggregation and the result takes
// another; the rest is a state row's header word and the rows in flight.
constexpr std::size_t BYTES_PER_AGG = 24;

std::size_t live() { return memory::detail::Manager::get().root().live(); }

// The highest live() a thread polling it sees while the object exists.
class PeakSampler {
   public:
    PeakSampler() : peak_(live()), thread_([this] { run(); }) {}
    ~PeakSampler() { stop(); }
    std::size_t stop() {
        if (thread_.joinable()) {
            stop_.store(true, std::memory_order_release);
            thread_.join();
        }
        return peak_.load(std::memory_order_acquire);
    }

   private:
    void run() {
        std::size_t high = peak_.load(std::memory_order_relaxed);
        while (!stop_.load(std::memory_order_acquire)) {
            high = std::max(high, live());
            peak_.store(high, std::memory_order_release);
            DFTRACER_UTILS_CPU_PAUSE();
        }
    }
    std::atomic<bool> stop_{false};
    std::atomic<std::size_t> peak_;
    dstd::Thread thread_;
};

struct Peak {
    std::size_t held;    // the most live bytes beyond the input
    std::size_t groups;  // rows of the result
};

// Sums of `aggs` nullable columns of the given type over ROWS rows with
// GROUPS keys, and what the group-by holds at most.
Peak group_by_peak(bool is_float, std::int64_t aggs) {
    std::mt19937_64 rng(1);
    dstd::Vector<std::int64_t> key(ROWS);
    for (auto& v : key) v = static_cast<std::int64_t>(rng() % GROUPS);
    DataFrame df;
    df.names.emplace_back("g");
    df.columns.push_back(Series::flat_i64(key.data(), ROWS));
    dstd::Vector<double> fval(ROWS);
    dstd::Vector<std::int64_t> ival(ROWS);
    dstd::Vector<std::uint8_t> valid((ROWS + 7) / 8);
    dstd::Vector<GroupAgg> specs;
    for (std::int64_t c = 0; c < aggs; ++c) {
        for (std::int64_t i = 0; i < ROWS; ++i) {
            fval[i] = static_cast<double>(rng() % 1000) / 7.0;
            ival[i] = static_cast<std::int64_t>(rng() % 1000);
            const bool present = rng() % 20 != 0;
            if (present)
                valid[i >> 3] |= static_cast<std::uint8_t>(1U << (i & 7));
            else
                valid[i >> 3] &= static_cast<std::uint8_t>(~(1U << (i & 7)));
        }
        const dstd::String name(dstd::fmt<"c{}">(c));
        df.names.emplace_back(dstd::StringView(name));
        df.columns.push_back(
            is_float ? Series::flat_f64(fval.data(), ROWS, valid.data())
                     : Series::flat_i64(ival.data(), ROWS, valid.data()));
        specs.push_back(
            GroupAgg{Agg::Sum, name, dstd::String(dstd::fmt<"s{}">(c))});
    }

    const std::size_t before = live();
    PeakSampler sampler;
    auto out = df.group_by("g", specs);
    const std::size_t peak = sampler.stop();
    REQUIRE(out.has_value());
    dstd::Vector<std::uint8_t> seen(GROUPS);
    std::int64_t distinct = 0;
    for (const std::int64_t k : key)
        if (!seen[static_cast<std::size_t>(k)]++) ++distinct;
    CHECK(out->num_rows() == distinct);
    REQUIRE(peak > before);
    return Peak{peak - before, static_cast<std::size_t>(distinct)};
}

// What each further aggregation adds to the peak, per group, as the difference
// between one aggregation and MANY: the key tables and the first-seen order
// are the same in both and drop out. The highest of a few runs, so the bound
// holds for the worst of them.
double bytes_per_group_per_agg(bool is_float) {
    constexpr int RUNS = 2;
    Peak one = group_by_peak(is_float, 1);
    Peak many = group_by_peak(is_float, MANY);
    for (int i = 1; i < RUNS; ++i) {
        one.held = std::max(one.held, group_by_peak(is_float, 1).held);
        many.held = std::max(many.held, group_by_peak(is_float, MANY).held);
    }
    REQUIRE(many.groups == one.groups);
    REQUIRE(many.held > one.held);
    return static_cast<double>(many.held - one.held) /
           static_cast<double>(one.groups) / static_cast<double>(MANY - 1);
}

}  // namespace

TEST_CASE("group_by holds a word per group per aggregate") {
    // Each worker owns a disjoint set of groups, so no lane holds a table of
    // them and nothing is merged or copied.
    CHECK(bytes_per_group_per_agg(false) <= static_cast<double>(BYTES_PER_AGG));
    CHECK(bytes_per_group_per_agg(true) <= static_cast<double>(BYTES_PER_AGG));
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

TEST_CASE("group_by folds integer aggregates over many groups exactly") {
    // Integer sums, extremes and counts do not depend on the order of rows, so
    // each worker owns a disjoint set of groups. The sums wrap for some groups,
    // which are null, and the groups come out in the order the rows show them.
    constexpr std::int64_t N = 262144;
    constexpr std::int64_t KEYS = 112656;
    std::mt19937_64 rng(5);
    dstd::Vector<std::int64_t> key(N), iv(N);
    dstd::Vector<std::uint8_t> valid((N + 7) / 8, 0);
    for (std::int64_t i = 0; i < N; ++i) {
        key[i] = static_cast<std::int64_t>(rng() % KEYS);
        const auto m =
            static_cast<std::int64_t>(rng() % (std::uint64_t{1} << 62));
        iv[i] = (rng() & 1) ? m : -m;
        if (rng() % 20)
            valid[i >> 3] |= static_cast<std::uint8_t>(1U << (i & 7));
    }
    DataFrame df;
    df.names = dstd::make_shared_strings({"g", "i"});
    df.columns.push_back(Series::flat_i64(key.data(), N));
    df.columns.push_back(Series::flat_i64(iv.data(), N, valid.data()));
    const dstd::Vector<GroupAgg> aggs{
        GroupAgg{Agg::Sum, "i", "si"}, GroupAgg{Agg::Min, "i", "lo"},
        GroupAgg{Agg::Max, "i", "hi"}, GroupAgg{Agg::Count, "i", "n"}};
    DataFrame out = dftu_utils_test::ok(df.group_by("g", aggs));

    struct Expect {
        dstd::int128_t si = 0;
        std::int64_t lo = INT64_MAX, hi = INT64_MIN, rows = 0, n = 0;
    };
    dstd::HashMap<std::int64_t, Expect> expect;
    dstd::Vector<std::int64_t> first_seen;
    for (std::int64_t i = 0; i < N; ++i) {
        const auto [it, fresh] = expect.try_emplace(key[i]);
        if (fresh) first_seen.push_back(key[i]);
        Expect& e = it->second;
        ++e.rows;
        if (!((valid[i >> 3] >> (i & 7)) & 1)) continue;
        e.si += iv[i];
        ++e.n;
        e.lo = std::min(e.lo, iv[i]);
        e.hi = std::max(e.hi, iv[i]);
    }
    REQUIRE(out.num_rows() == static_cast<std::int64_t>(first_seen.size()));
    std::int64_t wrapped = 0, bad = 0;
    for (std::int64_t g = 0; g < out.num_rows(); ++g) {
        const Expect& e = expect[first_seen[static_cast<std::size_t>(g)]];
        bool ok = out.column("g").data<std::int64_t>()[g] ==
                  first_seen[static_cast<std::size_t>(g)];
        const bool fits = e.si >= INT64_MIN && e.si <= INT64_MAX;
        wrapped += !fits;
        ok = ok && out.column("n").data<std::int64_t>()[g] == e.rows;
        if (e.n == 0) {
            ok = ok && out.column("lo").is_null(g) &&
                 out.column("hi").is_null(g);
        } else {
            ok = ok && out.column("si").is_null(g) == !fits &&
                 out.column("lo").data<std::int64_t>()[g] == e.lo &&
                 out.column("hi").data<std::int64_t>()[g] == e.hi;
            if (fits)
                ok = ok && out.column("si").data<std::int64_t>()[g] ==
                               static_cast<std::int64_t>(e.si);
        }
        bad += !ok;
    }
    CHECK(bad == 0);
    CHECK(wrapped > 100);
}

namespace {

// Float sums and counts over N rows with `keys` distinct keys, against a
// serial fold in row order: bit for bit where the by-key driver runs (`exact`),
// to a tolerance where the lanes do, which associate differently.
void check_float_sums(std::int64_t keys, bool exact) {
    constexpr std::int64_t N = 200000;
    std::mt19937_64 rng(11);
    dstd::Vector<std::int64_t> key(N);
    dstd::Vector<double> fv(N);
    dstd::Vector<std::uint8_t> valid((N + 7) / 8, 0);
    for (std::int64_t i = 0; i < N; ++i) {
        key[i] = static_cast<std::int64_t>(rng() % keys);
        fv[i] = static_cast<double>(rng() % 1000003) / 7.0 + 1e9;
        if (rng() % 20)
            valid[i >> 3] |= static_cast<std::uint8_t>(1U << (i & 7));
    }
    DataFrame df;
    df.names = dstd::make_shared_strings({"g", "f"});
    df.columns.push_back(Series::flat_i64(key.data(), N));
    df.columns.push_back(Series::flat_f64(fv.data(), N, valid.data()));
    const dstd::Vector<GroupAgg> aggs{GroupAgg{Agg::Sum, "f", "sf"},
                                      GroupAgg{Agg::Count, "f", "n"}};
    DataFrame out = dftu_utils_test::ok(df.group_by("g", aggs));

    dstd::HashMap<std::int64_t, double> sum;
    dstd::Vector<std::int64_t> first_seen;
    for (std::int64_t i = 0; i < N; ++i) {
        const auto [it, fresh] = sum.try_emplace(key[i], 0.0);
        if (fresh) first_seen.push_back(key[i]);
        if ((valid[i >> 3] >> (i & 7)) & 1) it->second += fv[i];
    }
    REQUIRE(out.num_rows() == static_cast<std::int64_t>(first_seen.size()));
    std::int64_t bad = 0;
    for (std::int64_t g = 0; g < out.num_rows(); ++g) {
        const double want = sum[first_seen[static_cast<std::size_t>(g)]];
        const double got = out.column("sf").data<double>()[g];
        const bool ok =
            out.column("g").data<std::int64_t>()[g] ==
                first_seen[static_cast<std::size_t>(g)] &&
            (exact ? got == want
                   : std::fabs(got - want) <= 1e-9 * std::fabs(want));
        bad += !ok;
    }
    CHECK(bad == 0);
}

}  // namespace

// The lanes start at once and lane 0's first 4096 rows estimate the groups;
// at about 10000 the lanes are dropped and the by-key driver runs.
TEST_CASE("group_by drops the lanes for many groups and sums in row order") {
    check_float_sums(60000, true);
}

TEST_CASE("group_by keeps the lanes for few groups") {
    check_float_sums(500, false);
}

TEST_CASE("group_by keeps the lanes just under the many-group estimate") {
    check_float_sums(8500, false);
}

TEST_CASE("group_by drops the lanes just over the many-group estimate") {
    check_float_sums(12000, true);
}
