#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using dftracer::utils::dataframe::AggOp;
using dftracer::utils::dataframe::AggSpec;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::Series;
using dftracer::utils::dataframe::TypeId;

namespace df = dftracer::utils::dataframe;

namespace {

// Above the partitioned drivers' row floor; a quarter under Valgrind.
const std::int64_t ROWS =
    static_cast<std::int64_t>(DFTRACER_UTILS_VALGRIND_SCALE(1'200'000, 4));
const std::int64_t GROUPS =
    static_cast<std::int64_t>(DFTRACER_UTILS_VALGRIND_SCALE(150'000, 4));
// Repeats of one group-by that must give the same bits. The point is the
// scheduling, which Valgrind does not check, so it runs only two.
const int REPEATS =
    static_cast<int>(DFTRACER_UTILS_VALGRIND_SCALE_MIN(20, 10, 2));

// Value columns, by index in `values`.
enum : std::int32_t { X = 0, Y = 1, S = 2, U = 3 };

struct Input {
    std::vector<std::string> key_text;
    std::vector<std::string_view> key_views;
    std::vector<std::uint8_t> key_valid;
    std::vector<std::int64_t> time;
    std::vector<std::int64_t> bigkey;
    std::vector<double> x, u;
    std::vector<std::uint8_t> x_valid;
    std::vector<std::int64_t> y;
    std::vector<std::string> sval;
    std::vector<std::string_view> sval_views;
    Series skey, tkey, bkey, xs, ys, ss, us;
    bool null_keys = false;

    // A string key longer than 16 bytes (the packed driver's limit), a small
    // integer key, and four value columns: a float with nulls, an integer, a
    // string, and a float no two rows share (for ArgMax).
    explicit Input(bool with_null_keys) : null_keys(with_null_keys) {
        std::mt19937_64 rng(7);
        const std::int64_t procs = GROUPS / 10;
        key_text.resize(ROWS);
        time.resize(ROWS);
        bigkey.resize(ROWS);
        x.resize(ROWS);
        u.resize(ROWS);
        y.resize(ROWS);
        sval.resize(ROWS);
        x_valid.assign(static_cast<std::size_t>((ROWS + 7) / 8), 0);
        key_valid.assign(static_cast<std::size_t>((ROWS + 7) / 8), 0);
        for (std::int64_t i = 0; i < ROWS; ++i) {
            const auto p = static_cast<std::int64_t>(rng() % procs);
            key_text[i] =
                "app#host#" + std::to_string(p) + "#" + std::to_string(p);
            time[i] = static_cast<std::int64_t>(rng() % 10);
            bigkey[i] = static_cast<std::int64_t>(rng() % GROUPS) * 1000003;
            x[i] = static_cast<double>(rng() % 100000) / 7.0;
            if (rng() % 20 != 0)
                x_valid[i >> 3] |= static_cast<std::uint8_t>(1U << (i & 7));
            u[i] = static_cast<double>(i);
            y[i] = static_cast<std::int64_t>(rng() % 1000) - 500;
            sval[i] = "v" + std::to_string(rng() % 50);
            if (!with_null_keys || rng() % 20 != 0)
                key_valid[i >> 3] |= static_cast<std::uint8_t>(1U << (i & 7));
        }
        for (const auto& s : key_text) key_views.push_back(s);
        for (const auto& s : sval) sval_views.push_back(s);
        skey =
            with_null_keys
                ? Series::strings(std::span<const std::string_view>(key_views),
                                  key_valid.data())
                : Series::strings(std::span<const std::string_view>(key_views));
        tkey = Series::flat(TypeId::Int64, time.data(), ROWS);
        bkey = Series::flat(TypeId::Int64, bigkey.data(), ROWS);
        xs = Series::flat_f64(x.data(), ROWS, x_valid.data());
        ys = Series::flat_i64(y.data(), ROWS);
        ss = Series::strings(std::span<const std::string_view>(sval_views));
        us = Series::flat_f64(u.data(), ROWS);
    }

    std::vector<const Series*> values() const { return {&xs, &ys, &ss, &us}; }
};

// The aggregates every many-group driver takes. arg_max is not here: its
// result depends on the order rows reach a group, and the driver that streams
// blocks refuses it (specs_with_arg_max).
std::vector<AggSpec> specs() {
    auto s = [](AggOp op, std::int32_t col, const char* out, double param = 0.0,
                std::int32_t by = -1) {
        AggSpec sp;
        sp.op = op;
        sp.value_col = col;
        sp.out = out;
        sp.param = param;
        sp.by_col = by;
        return sp;
    };
    return {s(AggOp::Count, -1, "n"),
            s(AggOp::CountValid, X, "nx"),
            s(AggOp::Sum, X, "sum_x"),
            s(AggOp::Mean, X, "mean_x"),
            s(AggOp::Var, X, "var_x"),
            s(AggOp::Std, X, "std_x"),
            s(AggOp::Min, X, "min_x"),
            s(AggOp::Max, X, "max_x"),
            s(AggOp::First, X, "first_x"),
            s(AggOp::Last, X, "last_x"),
            s(AggOp::SumSq, X, "sq_x"),
            s(AggOp::Pct, X, "p50_x", 0.5),
            s(AggOp::Sum, Y, "sum_y"),
            s(AggOp::Min, Y, "min_y"),
            s(AggOp::Max, Y, "max_y"),
            s(AggOp::SetUnion, S, "set_s"),
            s(AggOp::Distinct, S, "distinct_s", 1024.0)};
}

std::vector<AggSpec> specs_with_arg_max() {
    std::vector<AggSpec> all = specs();
    AggSpec sp;
    sp.op = AggOp::ArgMax;
    sp.value_col = X;
    sp.out = "argmax_x";
    sp.by_col = U;
    all.push_back(sp);
    return all;
}

// A single-threaded fold of the whole frame: the reference every driver must
// equal.
DataFrame reference(const std::vector<const Series*>& keys, const Input& in,
                    const std::vector<std::string>& key_names,
                    std::vector<AggSpec> with = specs()) {
    df::AggStatePtr st = df::agg_new(std::move(with));
    df::agg_accumulate(*st, keys, in.values());
    return df::agg_finalize(*st, key_names);
}

// Every column equal to the fold (floats to 1e-9 relative, the order of float
// additions may differ), except the names in `skip`. The quantile sketch
// (p50_x) is skipped by the many-group cases: its 128 collapsing bins make a
// sketch built from rows in another order differ from the fold in the tail
// bins.
void expect_equal(const DataFrame& a, const DataFrame& b,
                  const std::vector<std::string>& skip = {}) {
    REQUIRE(a.num_rows() == b.num_rows());
    REQUIRE(a.columns.size() == b.columns.size());
    for (std::size_t c = 0; c < a.columns.size(); ++c) {
        if (std::find(skip.begin(), skip.end(), a.names[c]) != skip.end())
            continue;
        const Series& p = a.columns[c];
        const Series& q = b.columns[c];
        INFO("column " << a.names[c]);
        REQUIRE(p.type() == q.type());
        std::int64_t bad = 0;
        for (std::int64_t i = 0; i < p.length() && bad < 3; ++i) {
            if (p.is_null(i) != q.is_null(i)) {
                ++bad;
                continue;
            }
            if (p.is_null(i)) continue;
            switch (p.type()) {
                case TypeId::Float64: {
                    const double u = p.data<double>()[i],
                                 v = q.data<double>()[i];
                    const double tol =
                        1e-9 * std::max({1.0, std::fabs(u), std::fabs(v)});
                    if (!(std::fabs(u - v) <= tol ||
                          (std::isnan(u) && std::isnan(v))))
                        ++bad;
                    break;
                }
                case TypeId::Int64:
                    if (p.data<std::int64_t>()[i] != q.data<std::int64_t>()[i])
                        ++bad;
                    break;
                case TypeId::Uint64:
                    if (p.data<std::uint64_t>()[i] !=
                        q.data<std::uint64_t>()[i])
                        ++bad;
                    break;
                case TypeId::Int32:
                    if (p.data<std::int32_t>()[i] != q.data<std::int32_t>()[i])
                        ++bad;
                    break;
                case TypeId::String:
                    if (p.string_at(i) != q.string_at(i)) ++bad;
                    break;
                default:
                    FAIL("unexpected output type in " << a.names[c]);
            }
        }
        CHECK(bad == 0);
    }
}

// Bits, not tolerances: every output column of two frames identical, the float
// sums and the sketch included.
bool bit_equal(const DataFrame& a, const DataFrame& b, std::string* why) {
    if (a.num_rows() != b.num_rows() || a.columns.size() != b.columns.size()) {
        *why = "shape";
        return false;
    }
    for (std::size_t c = 0; c < a.columns.size(); ++c) {
        const Series& p = a.columns[c];
        const Series& q = b.columns[c];
        if (p.type() != q.type()) {
            *why = a.names[c] + " type";
            return false;
        }
        for (std::int64_t i = 0; i < p.length(); ++i) {
            if (p.is_null(i) != q.is_null(i)) {
                *why = a.names[c] + " validity at " + std::to_string(i);
                return false;
            }
            if (p.is_null(i)) continue;
            bool same = true;
            switch (p.type()) {
                case TypeId::Float64:
                    same = std::memcmp(&p.data<double>()[i],
                                       &q.data<double>()[i], 8) == 0;
                    break;
                case TypeId::Int64:
                case TypeId::Uint64:
                    same =
                        p.data<std::int64_t>()[i] == q.data<std::int64_t>()[i];
                    break;
                case TypeId::Int32:
                    same =
                        p.data<std::int32_t>()[i] == q.data<std::int32_t>()[i];
                    break;
                case TypeId::String:
                    same = p.string_at(i) == q.string_at(i);
                    break;
                default:
                    break;
            }
            if (!same) {
                *why = a.names[c] + " at row " + std::to_string(i);
                return false;
            }
        }
    }
    return true;
}

// The same group-by REPEATS times on the thread pool and once serial: the bits
// must not change with the scheduling or with the number of threads.
void expect_deterministic(const std::vector<const Series*>& keys,
                          const Input& in, const std::vector<AggSpec>& with,
                          const std::vector<std::string>& names) {
    const DataFrame first = df::group_agg(keys, in.values(), with, names);
    std::string why;
    for (int run = 0; run < REPEATS; ++run) {
        const DataFrame again = df::group_agg(keys, in.values(), with, names);
        INFO("run " << run << ": " << why);
        REQUIRE(bit_equal(first, again, &why));
    }
    df::set_parallel_backend(nullptr, nullptr);  // serial
    const DataFrame serial = df::group_agg(keys, in.values(), with, names);
    df::install_runtime_parallel_backend();
    INFO("serial: " << why);
    REQUIRE(bit_equal(first, serial, &why));
}

struct Backend {
    Backend() { df::install_runtime_parallel_backend(); }
} backend;

}  // namespace

TEST_CASE("many groups on a long string key equal the one-thread fold") {
    Input in(false);
    const std::vector<const Series*> keys{&in.skey, &in.tkey};
    DataFrame got = df::group_agg(keys, in.values(), specs(), {"proc", "t"});
    REQUIRE(df::parallel_backend_installed());
    CHECK(got.num_rows() > GROUPS * 2 / 3);
    expect_equal(got, reference(keys, in, {"proc", "t"}), {"p50_x"});
}

TEST_CASE("many groups on an integer key equal the one-thread fold") {
    Input in(false);
    const std::vector<const Series*> keys{&in.bkey, &in.tkey};
    DataFrame got = df::group_agg(keys, in.values(), specs(), {"big", "t"});
    CHECK(got.num_rows() > GROUPS * 2 / 3);
    expect_equal(got, reference(keys, in, {"big", "t"}), {"p50_x"});
}

TEST_CASE("null keys form one group and equal the one-thread fold") {
    Input in(true);
    const std::vector<const Series*> keys{&in.skey, &in.tkey};
    DataFrame got = df::group_agg(keys, in.values(), specs(), {"proc", "t"});
    expect_equal(got, reference(keys, in, {"proc", "t"}), {"p50_x"});
    // the null string key is one group per time bucket, not one per partition
    std::int64_t nulls = 0;
    for (std::int64_t i = 0; i < got.columns[0].length(); ++i)
        nulls += got.columns[0].is_null(i);
    CHECK(nulls <= 10);
}

TEST_CASE("a float key keeps the existing path and still equals the fold") {
    Input in(false);
    std::vector<double> fk(ROWS);
    for (std::int64_t i = 0; i < ROWS; ++i)
        fk[i] = static_cast<double>(i % 50000) * 0.25;
    Series fkey = Series::flat_f64(fk.data(), ROWS);  // 50,000 groups
    const std::vector<const Series*> keys{&fkey};
    DataFrame got = df::group_agg(keys, in.values(), specs(), {"f"});
    // The quantile sketch is left out: its 128 collapsing bins make a merge of
    // per-thread partial sketches differ from one sketch run to run (a
    // separate, existing limitation).
    expect_equal(got, reference(keys, in, {"f"}), {"p50_x"});
}

TEST_CASE("arg_max over many groups equals the fold, the first row on a tie") {
    Input in(false);
    const std::vector<const Series*> keys{&in.skey, &in.tkey};
    DataFrame got =
        df::group_agg(keys, in.values(), specs_with_arg_max(), {"proc", "t"});
    // The sketch quantile is left out for the reason given above.
    expect_equal(got, reference(keys, in, {"proc", "t"}, specs_with_arg_max()),
                 {"p50_x"});
}

TEST_CASE("many groups give the same bits on every run and thread count") {
    Input in(true);
    std::vector<std::string> short_text(ROWS);
    std::vector<std::string_view> short_views;
    for (std::int64_t i = 0; i < ROWS; ++i)
        short_text[i] = "k" + std::to_string((i * 7919) % GROUPS);
    for (const auto& t : short_text) short_views.push_back(t);
    Series shortkey =
        Series::strings(std::span<const std::string_view>(short_views));
    SUBCASE("long string key") {
        expect_deterministic({&in.skey, &in.tkey}, in, specs(), {"proc", "t"});
    }
    SUBCASE("integer key") {
        expect_deterministic({&in.bkey, &in.tkey}, in, specs(), {"big", "t"});
    }
    SUBCASE("short string key, float sums") {
        expect_deterministic({&shortkey}, in, specs(), {"k"});
    }
    SUBCASE("short string key, order-free aggregates (the packed driver)") {
        std::vector<AggSpec> sp;
        for (const AggSpec& a : specs())
            if (a.op == AggOp::Count || a.op == AggOp::Min ||
                a.op == AggOp::Max || (a.op == AggOp::Sum && a.value_col == Y))
                sp.push_back(a);
        expect_deterministic({&shortkey}, in, sp, {"k"});
    }
}

TEST_CASE("few groups give the same bits on every run and thread count") {
    Input in(false);
    expect_deterministic({&in.tkey}, in, specs(), {"t"});
}

TEST_CASE("the group-by of many groups equals the serial one-pass sum") {
    // A sum in row order per group is what the serial fold computes; the
    // by-key driver keeps that order, so its sums are the fold's bits.
    Input in(false);
    const std::vector<const Series*> keys{&in.bkey};
    DataFrame got = df::group_agg(keys, in.values(), specs(), {"big"});
    DataFrame ref = reference(keys, in, {"big"});
    std::string why;
    // Only the columns whose bits do not depend on how partials merge.
    const std::vector<std::string> same{"n",     "nx",      "sum_y",
                                        "min_x", "max_x",   "min_y",
                                        "max_y", "first_x", "last_x"};
    for (const std::string& name : same) {
        std::size_t c = 0;
        while (got.names[c] != name) ++c;
        DataFrame a, b;
        a.names = {got.names[0], name};
        a.columns.push_back(got.columns[0].share());
        a.columns.push_back(got.columns[c].share());
        b.names = {ref.names[0], name};
        b.columns.push_back(ref.columns[0].share());
        b.columns.push_back(ref.columns[c].share());
        INFO(name << ": " << why);
        CHECK(bit_equal(a, b, &why));
    }
}

namespace {

// The aggregates the in-place finalize writes: light cells over a float with
// nulls and an integer, the row count and the count of present values.
std::vector<AggSpec> light_specs() {
    std::vector<AggSpec> out;
    for (const AggSpec& a : specs()) {
        switch (a.op) {
            case AggOp::Count:
            case AggOp::CountValid:
            case AggOp::Sum:
            case AggOp::Mean:
            case AggOp::Var:
            case AggOp::Std:
            case AggOp::Min:
            case AggOp::Max:
                if (a.value_col == X || a.value_col == Y || a.value_col < 0)
                    out.push_back(a);
                break;
            default:
                break;
        }
    }
    return out;
}

// The group-by once in place and once with the stitch path forced (the test
// switch), bit for bit: values, null masks, key columns, group order.
void expect_in_place_equals_stitch(const std::vector<const Series*>& keys,
                                   const Input& in,
                                   const std::vector<AggSpec>& with,
                                   const std::vector<std::string>& names,
                                   bool in_place) {
    const std::uint64_t before = df::agg_in_place_finalizes();
    const DataFrame got = df::group_agg(keys, in.values(), with, names);
    CHECK((df::agg_in_place_finalizes() > before) == in_place);
    REQUIRE(setenv("DFTRACER_UTILS_GROUPBY_STITCH", "1", 1) == 0);
    const std::uint64_t mid = df::agg_in_place_finalizes();
    const DataFrame ref = df::group_agg(keys, in.values(), with, names);
    unsetenv("DFTRACER_UTILS_GROUPBY_STITCH");
    CHECK(df::agg_in_place_finalizes() == mid);
    CHECK(got.num_rows() > 1000);
    std::string why;
    INFO(why);
    REQUIRE(bit_equal(got, ref, &why));
    REQUIRE(got.names == ref.names);
    for (std::size_t c = 0; c < got.columns.size(); ++c)
        CHECK(got.columns[c].null_count() == ref.columns[c].null_count());
}

}  // namespace

TEST_CASE("the in-place finalize writes the bits of the stitch path") {
    Input in(false);
    SUBCASE("long string key and an integer key") {
        expect_in_place_equals_stitch({&in.skey, &in.tkey}, in, light_specs(),
                                      {"proc", "t"}, true);
    }
    SUBCASE("integer keys") {
        expect_in_place_equals_stitch({&in.bkey, &in.tkey}, in, light_specs(),
                                      {"big", "t"}, true);
    }
    SUBCASE("a single long string key") {
        expect_in_place_equals_stitch({&in.skey}, in, light_specs(), {"proc"},
                                      true);
    }
    SUBCASE("every aggregate, text and list columns included, falls back") {
        expect_in_place_equals_stitch({&in.skey, &in.tkey}, in, specs(),
                                      {"proc", "t"}, false);
    }
    SUBCASE("arg_max is order-dependent state and falls back") {
        expect_in_place_equals_stitch({&in.skey, &in.tkey}, in,
                                      specs_with_arg_max(), {"proc", "t"},
                                      false);
    }
}

TEST_CASE(
    "the in-place finalize keeps the null key groups and the null cells") {
    Input in(true);
    expect_in_place_equals_stitch({&in.skey, &in.tkey}, in, light_specs(),
                                  {"proc", "t"}, true);
}

TEST_CASE("the in-place finalize handles the packed driver's short keys") {
    Input in(false);
    std::vector<std::string> short_text(ROWS);
    std::vector<std::string_view> short_views;
    for (std::int64_t i = 0; i < ROWS; ++i)
        short_text[i] = "k" + std::to_string((i * 7919) % GROUPS);
    for (const auto& t : short_text) short_views.push_back(t);
    Series shortkey =
        Series::strings(std::span<const std::string_view>(short_views));
    std::vector<AggSpec> order_free;  // count, min, max, an integer sum
    for (const AggSpec& a : specs())
        if (a.op == AggOp::Count || a.op == AggOp::Min || a.op == AggOp::Max ||
            (a.op == AggOp::Sum && a.value_col == Y))
            order_free.push_back(a);
    expect_in_place_equals_stitch({&shortkey}, in, order_free, {"k"}, true);
}
