// Benchmark: parallel group-by via the dataframe parallel_for seam.
//
// group_agg already fans out per-chunk AggStates through parallel_for and merges
// the partials. With no backend installed it runs serial; this bench installs a
// std::thread backend and reports group_by wall-clock at 1/2/4/8 workers so the
// speedup is measured, not assumed.
//
// Built only with DFTRACER_UTILS_BUILD_BENCHMARKS=ON; run manually. Use an
// optimized build (the dev preset, -O2) - a Debug build's numbers are noise.
//
//   dataframe_parallel_bench [rows] [groups]

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/field_stat.h>
#include <dftracer/utils/dataframe/kernels/field_stat.h>
#include <dftracer/utils/dataframe/kernels/stats.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dataframe/series.h>

#include <dftracer/utils/dataframe/batch_ops.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <utility>
#include <vector>

using namespace dftracer::utils::dataframe;

namespace {

// Drives a LazyFrame::collect() coroutine to completion on the default
// runtime; the bench installs its own thread backend but the runtime itself
// is single-threaded here, so this just blocks for the result.
DataFrame run(dftracer::utils::coro::CoroTask<DataFrame> t) {
    return dftracer::utils::default_runtime().submit(std::move(t)).get();
}

int g_threads = 1;

// Backend contract: call `body` once per grain-sized chunk (group_agg indexes
// its partials by begin/grain), distributing chunks across g_threads workers via
// an atomic counter (dynamic schedule).
void thread_backend(void*, std::int64_t n, std::int64_t grain,
                    void (*body)(void*, std::int64_t, std::int64_t),
                    void* bctx) {
    const std::int64_t nchunks = (n + grain - 1) / grain;
    std::atomic<std::int64_t> next{0};
    auto worker = [&]() {
        for (;;) {
            const std::int64_t k = next.fetch_add(1);
            if (k >= nchunks) break;
            const std::int64_t b = k * grain;
            const std::int64_t e = std::min(n, b + grain);
            body(bctx, b, e);
        }
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < g_threads; ++t) pool.emplace_back(worker);
    worker();
    for (auto& th : pool) th.join();
}

template <class T>
void do_not_optimize(T&& v) {
    asm volatile("" : : "r,m"(v) : "memory");
}

double time_group_by(const DataFrame& df, const std::vector<GroupAgg>& aggs,
                     int reps) {
    double best = 1e300;
    for (int r = 0; r < reps; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        DataFrame out = df.group_by("k", aggs);
        const auto t1 = std::chrono::steady_clock::now();
        do_not_optimize(out.num_rows());
        const double ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (ms < best) best = ms;
    }
    return best;
}

}  // namespace

int main(int argc, char** argv) {
    const std::int64_t rows = argc > 1 ? std::atoll(argv[1]) : 20'000'000;
    const std::int64_t groups = argc > 2 ? std::atoll(argv[2]) : 1000;

    std::vector<std::int64_t> k(static_cast<std::size_t>(rows));
    std::vector<std::int64_t> v(static_cast<std::size_t>(rows));
    std::vector<std::int64_t> s(static_cast<std::size_t>(rows));
    for (std::int64_t i = 0; i < rows; ++i) {
        k[static_cast<std::size_t>(i)] = i % groups;
        v[static_cast<std::size_t>(i)] = i;
        s[static_cast<std::size_t>(i)] = static_cast<std::int64_t>(
            (static_cast<std::uint64_t>(i) * 2654435761ULL) % 1000003ULL);
    }
    DataFrame df;
    df.names = {"k", "v", "s"};
    df.columns.push_back(Series::flat_i64(k.data(), rows));
    df.columns.push_back(Series::flat_i64(v.data(), rows));
    df.columns.push_back(Series::flat_i64(s.data(), rows));
    std::vector<GroupAgg> aggs{{Agg::Sum, "v", "sum", 0.0},
                               {Agg::Mean, "v", "mean", 0.0},
                               {Agg::Count, "", "cnt", 0.0}};

    std::printf("group_by: %lld rows, %lld groups, sum+mean+count\n",
                static_cast<long long>(rows), static_cast<long long>(groups));

    // Baselines for ops that are already SIMD (sort = Highway VQSort) or
    // bandwidth-bound, to judge whether parallelizing them is worth it.
    {
        set_parallel_backend(nullptr, nullptr);
        double s_sort = 1e300, s_desc = 1e300;
        for (int r = 0; r < 3; ++r) {
            auto t0 = std::chrono::steady_clock::now();
            DataFrame o = df.sort_by("v");
            auto t1 = std::chrono::steady_clock::now();
            do_not_optimize(o.num_rows());
            s_sort = std::min(
                s_sort,
                std::chrono::duration<double, std::milli>(t1 - t0).count());
            t0 = std::chrono::steady_clock::now();
            DataFrame d = df.describe();
            t1 = std::chrono::steady_clock::now();
            do_not_optimize(d.num_rows());
            s_desc = std::min(
                s_desc,
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        std::printf("baselines (serial): sort_by %.2f ms | describe %.2f ms\n",
                    s_sort, s_desc);
    }

    set_parallel_backend(nullptr, nullptr);  // serial baseline
    const double serial = time_group_by(df, aggs, 5);
    std::printf("  serial          : %8.2f ms  (1.00x)\n", serial);

    set_parallel_backend(thread_backend, nullptr);
    for (int t : {1, 2, 4, 8}) {
        g_threads = t;
        const double ms = time_group_by(df, aggs, 5);
        std::printf("  parallel %2d thr : %8.2f ms  (%.2fx)\n", t, ms,
                    serial / ms);
    }

    // The real installer: fan out through the core coroutine runtime.
    install_runtime_parallel_backend();
    const double rt = time_group_by(df, aggs, 5);
    std::printf("  runtime backend : %8.2f ms  (%.2fx)\n", rt, serial / rt);

    // Lazy group_by (GroupByCursor bounded parallel sink) over the same data.
    auto time_lazy = [&](int reps) {
        double best = 1e300;
        for (int r = 0; r < reps; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            DataFrame out = run(df.lazy().group_by("k", aggs).collect());
            const auto t1 = std::chrono::steady_clock::now();
            do_not_optimize(out.num_rows());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        return best;
    };
    set_parallel_backend(nullptr, nullptr);
    const double lz_ser = time_lazy(5);
    install_runtime_parallel_backend();
    const double lz_par = time_lazy(5);
    std::printf("lazy group_by: serial %8.2f ms | runtime %8.2f ms (%.2fx)\n",
                lz_ser, lz_par, lz_ser / lz_par);

    // Consolidation overhead probe: route through lazy with ONE giant morsel
    // (no chunking/concat). Shows the pure cursor-wrapping cost vs eager.
    auto one_morsel = [&]() {
        double best = 1e300;
        for (int r = 0; r < 5; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            DataFrame out = run(df.lazy().group_by("k", aggs).collect(rows));
            const auto t1 = std::chrono::steady_clock::now();
            do_not_optimize(out.num_rows());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        return best;
    };
    const double lz_one = one_morsel();
    std::printf("lazy group_by (1 morsel, runtime): %8.2f ms\n", lz_one);

    // sort_by with the parallel gather (take) enabled.
    auto time_sort = [&]() {
        double best = 1e300;
        for (int r = 0; r < 3; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            DataFrame out = df.sort_by("s");  // scattered key
            const auto t1 = std::chrono::steady_clock::now();
            do_not_optimize(out.num_rows());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        return best;
    };
    set_parallel_backend(nullptr, nullptr);
    const double sort_ser = time_sort();
    DataFrame sorted_serial = df.sort_by("s");
    install_runtime_parallel_backend();
    const double sort_par = time_sort();
    DataFrame sorted_par = df.sort_by("s");
    std::printf("sort_by: serial %8.2f ms | runtime %8.2f ms (%.2fx)\n",
                sort_ser, sort_par, sort_ser / sort_par);

    // Correctness: parallel sort must equal serial sort element-wise.
    const std::int64_t* a = sorted_serial.column("s").data<std::int64_t>();
    const std::int64_t* b = sorted_par.column("s").data<std::int64_t>();
    const std::int64_t* av = sorted_serial.column("v").data<std::int64_t>();
    const std::int64_t* bv = sorted_par.column("v").data<std::int64_t>();
    bool ok = sorted_serial.num_rows() == sorted_par.num_rows();
    for (std::int64_t i = 0; ok && i < sorted_par.num_rows(); ++i)
        ok = a[i] == b[i] && av[i] == bv[i];
    std::printf("sort_by correctness (parallel == serial): %s\n",
                ok ? "OK" : "MISMATCH");

    // Parallel reduction atom: field_stat_reduce over a 20M column.
    auto time_stat = [&]() {
        double best = 1e300;
        FieldStat fs;
        for (int r = 0; r < 5; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            fs = field_stat_reduce(df.column("v"), 0, rows);
            const auto t1 = std::chrono::steady_clock::now();
            do_not_optimize(fs.sum);
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        return best;
    };
    set_parallel_backend(nullptr, nullptr);
    const double st_ser = time_stat();
    install_runtime_parallel_backend();
    const double st_par = time_stat();
    std::printf("field_stat_reduce: serial %6.2f ms | runtime %6.2f ms (%.2fx)\n",
                st_ser, st_par, st_ser / st_par);

    // unique: parallel row-key gen + radix-partitioned lock-free dedup.
    auto time_unique = [&](int reps) {
        double best = 1e300;
        for (int r = 0; r < reps; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            DataFrame out = run(df.lazy().unique().collect());
            const auto t1 = std::chrono::steady_clock::now();
            do_not_optimize(out.num_rows());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        return best;
    };
    set_parallel_backend(nullptr, nullptr);
    const double uq_ser = time_unique(3);
    DataFrame uq_serial_out = run(df.lazy().unique().collect());
    install_runtime_parallel_backend();
    const double uq_par = time_unique(3);
    DataFrame uq_par_out = run(df.lazy().unique().collect());
    bool uq_ok = uq_serial_out.num_rows() == uq_par_out.num_rows();
    {
        const std::int64_t* uv = uq_serial_out.column("v").data<std::int64_t>();
        const std::int64_t* pv = uq_par_out.column("v").data<std::int64_t>();
        for (std::int64_t i = 0; uq_ok && i < uq_serial_out.num_rows(); ++i)
            uq_ok = uv[i] == pv[i];
    }
    std::printf("unique (all-distinct): serial %8.2f ms | runtime %8.2f ms "
                "(%.2fx) correctness: %s\n",
                uq_ser, uq_par, uq_ser / uq_par, uq_ok ? "OK" : "MISMATCH");
    ok = ok && uq_ok;

    // Duplicate-heavy scenario (single low-cardinality column): the case
    // is_duplicated / is_unique / unique are actually used for, and where the
    // radix-partitioned dedup / group_agg-count reuse pays off.
    DataFrame dup_df;
    dup_df.names = {"k"};
    dup_df.columns.push_back(Series::flat_i64(k.data(), rows));

    auto bit_at = [](const Series& s, std::int64_t idx) -> int {
        return (s.data<std::uint8_t>()[idx >> 3] >> (idx & 7)) & 1;
    };

    auto time_unique_dup = [&](int reps) {
        double best = 1e300;
        for (int r = 0; r < reps; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            DataFrame out = run(dup_df.lazy().unique().collect());
            const auto t1 = std::chrono::steady_clock::now();
            do_not_optimize(out.num_rows());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        return best;
    };
    set_parallel_backend(nullptr, nullptr);
    const double uqd_ser = time_unique_dup(3);
    DataFrame uqd_serial_out = run(dup_df.lazy().unique().collect());
    install_runtime_parallel_backend();
    const double uqd_par = time_unique_dup(3);
    DataFrame uqd_par_out = run(dup_df.lazy().unique().collect());
    bool uqd_ok = uqd_serial_out.num_rows() == uqd_par_out.num_rows();
    {
        const std::int64_t* uv = uqd_serial_out.column("k").data<std::int64_t>();
        const std::int64_t* pv = uqd_par_out.column("k").data<std::int64_t>();
        for (std::int64_t i = 0; uqd_ok && i < uqd_serial_out.num_rows(); ++i)
            uqd_ok = uv[i] == pv[i];
    }
    std::printf("unique (dup-heavy, %lld groups): serial %8.2f ms | runtime "
                "%8.2f ms (%.2fx) correctness: %s\n",
                static_cast<long long>(groups), uqd_ser, uqd_par,
                uqd_ser / uqd_par, uqd_ok ? "OK" : "MISMATCH");
    ok = ok && uqd_ok;

    auto time_isdup = [&](bool want_unique, int reps) {
        double best = 1e300;
        for (int r = 0; r < reps; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            DataFrame out = run(want_unique
                               ? dup_df.lazy().is_unique().collect()
                               : dup_df.lazy().is_duplicated().collect());
            const auto t1 = std::chrono::steady_clock::now();
            do_not_optimize(out.num_rows());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        return best;
    };

    set_parallel_backend(nullptr, nullptr);
    const double dup_ser = time_isdup(false, 3);
    DataFrame dup_serial_out = run(dup_df.lazy().is_duplicated().collect());
    install_runtime_parallel_backend();
    const double dup_par = time_isdup(false, 3);
    DataFrame dup_par_out = run(dup_df.lazy().is_duplicated().collect());
    bool dup_ok = dup_serial_out.num_rows() == dup_par_out.num_rows();
    {
        const Series& sa = dup_serial_out.column("is_duplicated");
        const Series& sb = dup_par_out.column("is_duplicated");
        for (std::int64_t i = 0; dup_ok && i < dup_serial_out.num_rows(); ++i)
            dup_ok = bit_at(sa, i) == bit_at(sb, i);
    }
    std::printf("is_duplicated: serial %8.2f ms | runtime %8.2f ms (%.2fx) "
                "correctness: %s\n",
                dup_ser, dup_par, dup_ser / dup_par, dup_ok ? "OK" : "MISMATCH");
    ok = ok && dup_ok;

    set_parallel_backend(nullptr, nullptr);
    const double uni_ser = time_isdup(true, 3);
    DataFrame uni_serial_out = run(dup_df.lazy().is_unique().collect());
    install_runtime_parallel_backend();
    const double uni_par = time_isdup(true, 3);
    DataFrame uni_par_out = run(dup_df.lazy().is_unique().collect());
    bool uni_ok = uni_serial_out.num_rows() == uni_par_out.num_rows();
    {
        const Series& sa = uni_serial_out.column("is_unique");
        const Series& sb = uni_par_out.column("is_unique");
        for (std::int64_t i = 0; uni_ok && i < uni_serial_out.num_rows(); ++i)
            uni_ok = bit_at(sa, i) == bit_at(sb, i);
    }
    std::printf("is_unique: serial %8.2f ms | runtime %8.2f ms (%.2fx) "
                "correctness: %s\n",
                uni_ser, uni_par, uni_ser / uni_par, uni_ok ? "OK" : "MISMATCH");
    ok = ok && uni_ok;

    // String predicate row loop (regex / LIKE): the compute-bound per-row
    // scan parallelized on 8-row (byte) chunk boundaries in string_ops.cpp.
    const std::int64_t srows = 5'000'000;
    std::vector<std::string> str_vals(static_cast<std::size_t>(srows));
    for (std::int64_t i = 0; i < srows; ++i)
        str_vals[static_cast<std::size_t>(i)] =
            "event_" + std::to_string(i % 97) + "_tag" + std::to_string(i % 13);
    Series str_col = Series::strings(str_vals);

    auto time_regex = [&](int reps) {
        double best = 1e300;
        for (int r = 0; r < reps; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            Series mask = str_col.str_matches("event_[0-9]+_tag1.*");
            const auto t1 = std::chrono::steady_clock::now();
            do_not_optimize(mask.length());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        return best;
    };
    set_parallel_backend(nullptr, nullptr);
    const double re_ser = time_regex(3);
    Series re_serial_out = str_col.str_matches("event_[0-9]+_tag1.*");
    install_runtime_parallel_backend();
    const double re_par = time_regex(3);
    Series re_par_out = str_col.str_matches("event_[0-9]+_tag1.*");
    bool re_ok = re_serial_out.length() == re_par_out.length();
    for (std::int64_t i = 0; re_ok && i < re_serial_out.length(); ++i)
        re_ok = bit_at(re_serial_out, i) == bit_at(re_par_out, i);
    std::printf("str_matches (regex): serial %8.2f ms | runtime %8.2f ms "
                "(%.2fx) correctness: %s\n",
                re_ser, re_par, re_ser / re_par, re_ok ? "OK" : "MISMATCH");
    ok = ok && re_ok;

    auto time_like = [&](int reps) {
        double best = 1e300;
        for (int r = 0; r < reps; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            Series mask = str_col.str_like("event_%_tag1?");
            const auto t1 = std::chrono::steady_clock::now();
            do_not_optimize(mask.length());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        return best;
    };
    set_parallel_backend(nullptr, nullptr);
    const double lk_ser = time_like(3);
    Series lk_serial_out = str_col.str_like("event_%_tag1?");
    install_runtime_parallel_backend();
    const double lk_par = time_like(3);
    Series lk_par_out = str_col.str_like("event_%_tag1?");
    bool lk_ok = lk_serial_out.length() == lk_par_out.length();
    for (std::int64_t i = 0; lk_ok && i < lk_serial_out.length(); ++i)
        lk_ok = bit_at(lk_serial_out, i) == bit_at(lk_par_out, i);
    std::printf("str_like (glob): serial %8.2f ms | runtime %8.2f ms (%.2fx) "
                "correctness: %s\n",
                lk_ser, lk_par, lk_ser / lk_par, lk_ok ? "OK" : "MISMATCH");
    ok = ok && lk_ok;

    // quantile/median: the full VQSort over a wide numeric column.
    Series qcol = df.column("s");  // scattered i64 -> cast to f64 internally
    auto time_quantile = [&](int reps) {
        double best = 1e300;
        double r = 0.0;
        for (int i = 0; i < reps; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            r = quantile(qcol, 0.5);
            const auto t1 = std::chrono::steady_clock::now();
            do_not_optimize(r);
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        return best;
    };
    set_parallel_backend(nullptr, nullptr);
    const double q_ser = time_quantile(5);
    const double q_ser_val = quantile(qcol, 0.5);
    install_runtime_parallel_backend();
    const double q_par = time_quantile(5);
    const double q_par_val = quantile(qcol, 0.5);
    bool q_ok = q_ser_val == q_par_val;
    std::printf("quantile(0.5): serial %8.2f ms | runtime %8.2f ms (%.2fx) "
                "correctness: %s\n",
                q_ser, q_par, q_ser / q_par, q_ok ? "OK" : "MISMATCH");
    ok = ok && q_ok;

    // rolling_var: O(n*window) with each row's window independent.
    const std::int64_t rwin = 64;
    auto time_rolling_var = [&](int reps) {
        double best = 1e300;
        Series r;
        for (int i = 0; i < reps; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            r = qcol.rolling_var(rwin);
            const auto t1 = std::chrono::steady_clock::now();
            do_not_optimize(r.length());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        return r;
    };
    set_parallel_backend(nullptr, nullptr);
    double rv_ser = 1e300;
    {
        const auto t0 = std::chrono::steady_clock::now();
        Series r = qcol.rolling_var(rwin);
        const auto t1 = std::chrono::steady_clock::now();
        rv_ser = std::chrono::duration<double, std::milli>(t1 - t0).count();
        do_not_optimize(r.length());
    }
    Series rv_serial_out = time_rolling_var(3);
    install_runtime_parallel_backend();
    Series rv_par_out = time_rolling_var(3);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) {
        Series r = qcol.rolling_var(rwin);
        do_not_optimize(r.length());
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double rv_par =
        std::chrono::duration<double, std::milli>(t1 - t0).count() / 3.0;
    bool rv_ok = rv_serial_out.length() == rv_par_out.length();
    {
        const double* a2 = rv_serial_out.data<double>();
        const double* b2 = rv_par_out.data<double>();
        for (std::int64_t i = 0; rv_ok && i < rv_serial_out.length(); ++i) {
            const bool na = rv_serial_out.is_null(i);
            const bool nb = rv_par_out.is_null(i);
            if (na != nb) {
                rv_ok = false;
            } else if (!na) {
                rv_ok = std::abs(a2[i] - b2[i]) < 1e-6 * (1.0 + std::abs(a2[i]));
            }
        }
    }
    std::printf("rolling_var(w=%lld): serial %8.2f ms | runtime %8.2f ms "
                "(%.2fx) correctness: %s\n",
                static_cast<long long>(rwin), rv_ser, rv_par, rv_ser / rv_par,
                rv_ok ? "OK" : "MISMATCH");
    ok = ok && rv_ok;

    // topk_indices (via top_k): SIMD partial sort of a wide numeric column.
    const std::int64_t topk_k = 1000;
    auto time_topk = [&](int reps) {
        double best = 1e300;
        Series r;
        for (int i = 0; i < reps; ++i) {
            const auto tt0 = std::chrono::steady_clock::now();
            r = qcol.top_k(topk_k);
            const auto tt1 = std::chrono::steady_clock::now();
            do_not_optimize(r.length());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(tt1 - tt0).count());
        }
        return r;
    };
    set_parallel_backend(nullptr, nullptr);
    Series tk_serial_out = time_topk(3);
    double tk_ser = 1e300;
    for (int i = 0; i < 3; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = qcol.top_k(topk_k);
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        tk_ser = std::min(
            tk_ser, std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    install_runtime_parallel_backend();
    Series tk_par_out = time_topk(3);
    double tk_par = 1e300;
    for (int i = 0; i < 3; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = qcol.top_k(topk_k);
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        tk_par = std::min(
            tk_par, std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    bool tk_ok = tk_serial_out.length() == tk_par_out.length();
    {
        std::vector<std::int64_t> sv, pv;
        for (std::int64_t i = 0; i < tk_serial_out.length(); ++i)
            sv.push_back(tk_serial_out.data<std::int64_t>()[i]);
        for (std::int64_t i = 0; i < tk_par_out.length(); ++i)
            pv.push_back(tk_par_out.data<std::int64_t>()[i]);
        std::sort(sv.begin(), sv.end());
        std::sort(pv.begin(), pv.end());
        tk_ok = tk_ok && sv == pv;
    }
    std::printf("top_k(k=%lld): serial %8.2f ms | runtime %8.2f ms (%.2fx) "
                "correctness: %s\n",
                static_cast<long long>(topk_k), tk_ser, tk_par, tk_ser / tk_par,
                tk_ok ? "OK" : "MISMATCH");
    ok = ok && tk_ok;

    // to_dummies: O(n*d) nested string-eq loop fixed to an O(n) bucket pass.
    // d dummy columns of n rows each, so keep both bounded (d * n int8 cells).
    const std::int64_t drows = std::min<std::int64_t>(rows, 4'000'000);
    const std::int64_t dgroups = std::min<std::int64_t>(groups, 64);
    DataFrame dummy_df;
    dummy_df.names = {"cat"};
    {
        std::vector<std::string> cats(static_cast<std::size_t>(drows));
        for (std::int64_t i = 0; i < drows; ++i)
            cats[static_cast<std::size_t>(i)] =
                "c" + std::to_string(i % dgroups);
        dummy_df.columns.push_back(Series::strings(cats));
    }
    auto time_dummies = [&](int reps) {
        double best = 1e300;
        DataFrame r;
        for (int i = 0; i < reps; ++i) {
            const auto tt0 = std::chrono::steady_clock::now();
            r = dummy_df.to_dummies("cat");
            const auto tt1 = std::chrono::steady_clock::now();
            do_not_optimize(r.num_rows());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(tt1 - tt0).count());
        }
        return r;
    };
    set_parallel_backend(nullptr, nullptr);
    DataFrame td_serial_out = time_dummies(1);
    const double td_ser = [&] {
        const auto tt0 = std::chrono::steady_clock::now();
        DataFrame r = dummy_df.to_dummies("cat");
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.num_rows());
        return std::chrono::duration<double, std::milli>(tt1 - tt0).count();
    }();
    install_runtime_parallel_backend();
    DataFrame td_par_out = time_dummies(3);
    double td_par = 1e300;
    for (int i = 0; i < 3; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        DataFrame r = dummy_df.to_dummies("cat");
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.num_rows());
        td_par = std::min(
            td_par, std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    bool td_ok = td_serial_out.num_rows() == td_par_out.num_rows() &&
                td_serial_out.num_columns() == td_par_out.num_columns();
    for (std::int64_t c = 1;
        td_ok && c < static_cast<std::int64_t>(td_serial_out.num_columns());
        ++c) {
        const Series& a2 = td_serial_out.columns[static_cast<std::size_t>(c)];
        const Series& b2 = td_par_out.columns[static_cast<std::size_t>(c)];
        for (std::int64_t i = 0; td_ok && i < a2.length(); ++i)
            td_ok = a2.data<std::int8_t>()[i] == b2.data<std::int8_t>()[i];
    }
    std::printf("to_dummies (%lld groups): serial %8.2f ms | runtime %8.2f ms "
                "(%.2fx) correctness: %s\n",
                static_cast<long long>(groups), td_ser, td_par, td_ser / td_par,
                td_ok ? "OK" : "MISMATCH");
    ok = ok && td_ok;

    // hash_partition: per-row multi-col hash into shared buckets.
    auto time_hp = [&](int reps) {
        double best = 1e300;
        std::vector<DataFrame> r;
        for (int i = 0; i < reps; ++i) {
            const auto tt0 = std::chrono::steady_clock::now();
            r = hash_partition(dummy_df, {"cat"}, 64);
            const auto tt1 = std::chrono::steady_clock::now();
            std::int64_t total = 0;
            for (const auto& p : r) total += p.num_rows();
            do_not_optimize(total);
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(tt1 - tt0).count());
        }
        return r;
    };
    set_parallel_backend(nullptr, nullptr);
    std::vector<DataFrame> hp_serial_out = time_hp(3);
    double hp_ser = 1e300;
    for (int i = 0; i < 3; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        std::vector<DataFrame> r = hash_partition(dummy_df, {"cat"}, 64);
        const auto tt1 = std::chrono::steady_clock::now();
        std::int64_t total = 0;
        for (const auto& p : r) total += p.num_rows();
        do_not_optimize(total);
        hp_ser = std::min(
            hp_ser, std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    install_runtime_parallel_backend();
    std::vector<DataFrame> hp_par_out = time_hp(3);
    double hp_par = 1e300;
    for (int i = 0; i < 3; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        std::vector<DataFrame> r = hash_partition(dummy_df, {"cat"}, 64);
        const auto tt1 = std::chrono::steady_clock::now();
        std::int64_t total = 0;
        for (const auto& p : r) total += p.num_rows();
        do_not_optimize(total);
        hp_par = std::min(
            hp_par, std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    std::int64_t hp_ser_total = 0, hp_par_total = 0;
    for (const auto& p : hp_serial_out) hp_ser_total += p.num_rows();
    for (const auto& p : hp_par_out) hp_par_total += p.num_rows();
    bool hp_ok = hp_ser_total == drows && hp_par_total == drows &&
                hp_serial_out.size() == hp_par_out.size();
    std::printf("hash_partition (64 parts): serial %8.2f ms | runtime %8.2f "
                "ms (%.2fx) correctness: %s\n",
                hp_ser, hp_par, hp_ser / hp_par, hp_ok ? "OK" : "MISMATCH");
    ok = ok && hp_ok;

    // is_in: general (string) path, no SIMD fast path.
    Series needles = Series::strings({"c1", "c5", "c9", "c13"});
    auto time_is_in = [&](int reps) {
        double best = 1e300;
        Series r;
        for (int i = 0; i < reps; ++i) {
            const auto tt0 = std::chrono::steady_clock::now();
            r = dummy_df.column("cat").is_in(needles);
            const auto tt1 = std::chrono::steady_clock::now();
            do_not_optimize(r.length());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(tt1 - tt0).count());
        }
        return r;
    };
    set_parallel_backend(nullptr, nullptr);
    Series iin_serial_out = time_is_in(3);
    double iin_ser = 1e300;
    for (int i = 0; i < 3; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = dummy_df.column("cat").is_in(needles);
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        iin_ser = std::min(
            iin_ser, std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    install_runtime_parallel_backend();
    Series iin_par_out = time_is_in(3);
    double iin_par = 1e300;
    for (int i = 0; i < 3; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = dummy_df.column("cat").is_in(needles);
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        iin_par = std::min(
            iin_par, std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    bool iin_ok = iin_serial_out.length() == iin_par_out.length();
    for (std::int64_t i = 0; iin_ok && i < iin_serial_out.length(); ++i)
        iin_ok = bit_at(iin_serial_out, i) == bit_at(iin_par_out, i);
    std::printf("is_in (string): serial %8.2f ms | runtime %8.2f ms (%.2fx) "
                "correctness: %s\n",
                iin_ser, iin_par, iin_ser / iin_par, iin_ok ? "OK" : "MISMATCH");
    ok = ok && iin_ok;

    // sort_by_multi: std::stable_sort of indices with a multi-col comparator.
    DataFrame multi_df;
    multi_df.names = {"k", "v"};
    multi_df.columns.push_back(Series::flat_i64(k.data(), rows));
    multi_df.columns.push_back(Series::flat_i64(v.data(), rows));
    auto time_sbm = [&](int reps) {
        double best = 1e300;
        DataFrame r;
        for (int i = 0; i < reps; ++i) {
            const auto tt0 = std::chrono::steady_clock::now();
            r = sort_by_multi(multi_df, {"k", "v"}, false);
            const auto tt1 = std::chrono::steady_clock::now();
            do_not_optimize(r.num_rows());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(tt1 - tt0).count());
        }
        return r;
    };
    set_parallel_backend(nullptr, nullptr);
    DataFrame sbm_serial_out = time_sbm(3);
    double sbm_ser = 1e300;
    for (int i = 0; i < 3; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        DataFrame r = sort_by_multi(multi_df, {"k", "v"}, false);
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.num_rows());
        sbm_ser = std::min(
            sbm_ser, std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    install_runtime_parallel_backend();
    DataFrame sbm_par_out = time_sbm(3);
    double sbm_par = 1e300;
    for (int i = 0; i < 3; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        DataFrame r = sort_by_multi(multi_df, {"k", "v"}, false);
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.num_rows());
        sbm_par = std::min(
            sbm_par, std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    bool sbm_ok = sbm_serial_out.num_rows() == sbm_par_out.num_rows();
    {
        const std::int64_t* ka =
            sbm_serial_out.column("k").data<std::int64_t>();
        const std::int64_t* kb = sbm_par_out.column("k").data<std::int64_t>();
        const std::int64_t* va =
            sbm_serial_out.column("v").data<std::int64_t>();
        const std::int64_t* vb = sbm_par_out.column("v").data<std::int64_t>();
        for (std::int64_t i = 0; sbm_ok && i < sbm_serial_out.num_rows(); ++i)
            sbm_ok = ka[i] == kb[i] && va[i] == vb[i];
    }
    std::printf("sort_by_multi: serial %8.2f ms | runtime %8.2f ms (%.2fx) "
                "correctness: %s\n",
                sbm_ser, sbm_par, sbm_ser / sbm_par, sbm_ok ? "OK" : "MISMATCH");
    ok = ok && sbm_ok;

    // cumsum: two-pass parallel prefix scan (dense, no nulls) over the wide
    // scattered i64 column.
    auto time_cumsum = [&](int reps) {
        double best = 1e300;
        Series r;
        for (int i = 0; i < reps; ++i) {
            const auto tt0 = std::chrono::steady_clock::now();
            r = qcol.cumsum();
            const auto tt1 = std::chrono::steady_clock::now();
            do_not_optimize(r.length());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(tt1 - tt0).count());
        }
        return r;
    };
    set_parallel_backend(nullptr, nullptr);
    Series cs_serial_out = time_cumsum(3);
    double cs_ser = 1e300;
    for (int i = 0; i < 3; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = qcol.cumsum();
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        cs_ser = std::min(
            cs_ser, std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    install_runtime_parallel_backend();
    Series cs_par_out = time_cumsum(3);
    double cs_par = 1e300;
    for (int i = 0; i < 3; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = qcol.cumsum();
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        cs_par = std::min(
            cs_par, std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    bool cs_ok = cs_serial_out.length() == cs_par_out.length();
    {
        const std::int64_t* a2 = cs_serial_out.data<std::int64_t>();
        const std::int64_t* b2 = cs_par_out.data<std::int64_t>();
        for (std::int64_t i = 0; cs_ok && i < cs_serial_out.length(); ++i)
            cs_ok = a2[i] == b2[i];
    }
    std::printf("cumsum: serial %8.2f ms | runtime %8.2f ms (%.2fx) "
                "correctness: %s\n",
                cs_ser, cs_par, cs_ser / cs_par, cs_ok ? "OK" : "MISMATCH");
    ok = ok && cs_ok;

    // cummax: same scan, max-combine.
    auto time_cummax = [&](int reps) {
        double best = 1e300;
        Series r;
        for (int i = 0; i < reps; ++i) {
            const auto tt0 = std::chrono::steady_clock::now();
            r = qcol.cummax();
            const auto tt1 = std::chrono::steady_clock::now();
            do_not_optimize(r.length());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(tt1 - tt0).count());
        }
        return r;
    };
    set_parallel_backend(nullptr, nullptr);
    Series cx_serial_out = time_cummax(3);
    double cx_ser = 1e300;
    for (int i = 0; i < 3; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = qcol.cummax();
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        cx_ser = std::min(
            cx_ser, std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    install_runtime_parallel_backend();
    Series cx_par_out = time_cummax(3);
    double cx_par = 1e300;
    for (int i = 0; i < 3; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = qcol.cummax();
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        cx_par = std::min(
            cx_par, std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    bool cx_ok = cx_serial_out.length() == cx_par_out.length();
    {
        const std::int64_t* a2 = cx_serial_out.data<std::int64_t>();
        const std::int64_t* b2 = cx_par_out.data<std::int64_t>();
        for (std::int64_t i = 0; cx_ok && i < cx_serial_out.length(); ++i)
            cx_ok = a2[i] == b2[i];
    }
    std::printf("cummax: serial %8.2f ms | runtime %8.2f ms (%.2fx) "
                "correctness: %s\n",
                cx_ser, cx_par, cx_ser / cx_par, cx_ok ? "OK" : "MISMATCH");
    ok = ok && cx_ok;

    // Elementwise add: two 20M Float64 columns. Bandwidth-bound - measure
    // whether parallel_for over row ranges actually wins on this machine.
    std::vector<double> f1(static_cast<std::size_t>(rows)),
        f2(static_cast<std::size_t>(rows));
    for (std::int64_t i = 0; i < rows; ++i) {
        f1[static_cast<std::size_t>(i)] = static_cast<double>(i);
        f2[static_cast<std::size_t>(i)] = static_cast<double>(rows - i);
    }
    Series fa = Series::flat_f64(f1.data(), rows);
    Series fb = Series::flat_f64(f2.data(), rows);
    auto time_add = [&](int reps) {
        double best = 1e300;
        Series r;
        for (int i = 0; i < reps; ++i) {
            const auto tt0 = std::chrono::steady_clock::now();
            r = fa + fb;
            const auto tt1 = std::chrono::steady_clock::now();
            do_not_optimize(r.length());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(tt1 - tt0).count());
        }
        return r;
    };
    set_parallel_backend(nullptr, nullptr);
    Series add_serial_out = time_add(5);
    double add_ser = 1e300;
    for (int i = 0; i < 5; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = fa + fb;
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        add_ser = std::min(
            add_ser,
            std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    install_runtime_parallel_backend();
    Series add_par_out = time_add(5);
    double add_par = 1e300;
    for (int i = 0; i < 5; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = fa + fb;
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        add_par = std::min(
            add_par,
            std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    bool add_ok = add_serial_out.length() == add_par_out.length();
    {
        const double* a2 = add_serial_out.data<double>();
        const double* b2 = add_par_out.data<double>();
        for (std::int64_t i = 0; add_ok && i < add_serial_out.length(); ++i)
            add_ok = a2[i] == b2[i];
    }
    std::printf("add (2x20M f64): serial %8.2f ms | runtime %8.2f ms (%.2fx) "
                "correctness: %s\n",
                add_ser, add_par, add_ser / add_par, add_ok ? "OK" : "MISMATCH");
    ok = ok && add_ok;

    // Cast i64 -> f64 over 20M rows: bandwidth-bound single-pass convert.
    auto time_cast = [&](int reps) {
        double best = 1e300;
        Series r;
        for (int i = 0; i < reps; ++i) {
            const auto tt0 = std::chrono::steady_clock::now();
            r = df.column("v").cast(TypeId::Float64);
            const auto tt1 = std::chrono::steady_clock::now();
            do_not_optimize(r.length());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(tt1 - tt0).count());
        }
        return r;
    };
    set_parallel_backend(nullptr, nullptr);
    Series cast_serial_out = time_cast(5);
    double cast_ser = 1e300;
    for (int i = 0; i < 5; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = df.column("v").cast(TypeId::Float64);
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        cast_ser = std::min(
            cast_ser,
            std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    install_runtime_parallel_backend();
    Series cast_par_out = time_cast(5);
    double cast_par = 1e300;
    for (int i = 0; i < 5; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = df.column("v").cast(TypeId::Float64);
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        cast_par = std::min(
            cast_par,
            std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    bool cast_ok = cast_serial_out.length() == cast_par_out.length();
    {
        const double* a2 = cast_serial_out.data<double>();
        const double* b2 = cast_par_out.data<double>();
        for (std::int64_t i = 0; cast_ok && i < cast_serial_out.length(); ++i)
            cast_ok = a2[i] == b2[i];
    }
    std::printf("cast i64->f64 (20M): serial %8.2f ms | runtime %8.2f ms "
                "(%.2fx) correctness: %s\n",
                cast_ser, cast_par, cast_ser / cast_par,
                cast_ok ? "OK" : "MISMATCH");
    ok = ok && cast_ok;

    // concat_columns: many large parts, each a disjoint memcpy destination.
    const std::int64_t nparts = 256;
    const std::int64_t part_len = rows / nparts;
    std::vector<Series> parts;
    parts.reserve(static_cast<std::size_t>(nparts));
    for (std::int64_t p = 0; p < nparts; ++p)
        parts.push_back(
            Series::flat_i64(v.data() + p * part_len, part_len));
    std::vector<const Series*> part_ptrs;
    for (const Series& part : parts) part_ptrs.push_back(&part);
    auto time_concat = [&](int reps) {
        double best = 1e300;
        Series r;
        for (int i = 0; i < reps; ++i) {
            const auto tt0 = std::chrono::steady_clock::now();
            r = concat_columns(part_ptrs);
            const auto tt1 = std::chrono::steady_clock::now();
            do_not_optimize(r.length());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(tt1 - tt0).count());
        }
        return r;
    };
    set_parallel_backend(nullptr, nullptr);
    Series ccat_serial_out = time_concat(5);
    double ccat_ser = 1e300;
    for (int i = 0; i < 5; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = concat_columns(part_ptrs);
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        ccat_ser = std::min(
            ccat_ser,
            std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    install_runtime_parallel_backend();
    Series ccat_par_out = time_concat(5);
    double ccat_par = 1e300;
    for (int i = 0; i < 5; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = concat_columns(part_ptrs);
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        ccat_par = std::min(
            ccat_par,
            std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    bool ccat_ok = ccat_serial_out.length() == ccat_par_out.length();
    {
        const std::int64_t* a2 = ccat_serial_out.data<std::int64_t>();
        const std::int64_t* b2 = ccat_par_out.data<std::int64_t>();
        for (std::int64_t i = 0; ccat_ok && i < ccat_serial_out.length(); ++i)
            ccat_ok = a2[i] == b2[i];
    }
    std::printf("concat_columns (%lld parts): serial %8.2f ms | runtime "
                "%8.2f ms (%.2fx) correctness: %s\n",
                static_cast<long long>(nparts), ccat_ser, ccat_par,
                ccat_ser / ccat_par, ccat_ok ? "OK" : "MISMATCH");
    ok = ok && ccat_ok;

    // interpolate: nulls every 32 rows create many independent short runs.
    std::vector<double> interp_v(static_cast<std::size_t>(rows));
    std::vector<std::uint8_t> interp_valid(
        static_cast<std::size_t>((rows + 7) / 8), 0xFF);
    for (std::int64_t i = 0; i < rows; ++i) {
        interp_v[static_cast<std::size_t>(i)] = static_cast<double>(i % 1000);
        if (i % 32 != 0)
            interp_valid[static_cast<std::size_t>(i >> 3)] &=
                ~(1u << (i & 7));
    }
    Series interp_col =
        Series::flat(TypeId::Float64, interp_v.data(), rows, interp_valid.data());
    auto time_interp = [&](int reps) {
        double best = 1e300;
        Series r;
        for (int i = 0; i < reps; ++i) {
            const auto tt0 = std::chrono::steady_clock::now();
            r = interp_col.interpolate();
            const auto tt1 = std::chrono::steady_clock::now();
            do_not_optimize(r.length());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(tt1 - tt0).count());
        }
        return r;
    };
    set_parallel_backend(nullptr, nullptr);
    Series interp_serial_out = time_interp(5);
    double interp_ser = 1e300;
    for (int i = 0; i < 5; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = interp_col.interpolate();
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        interp_ser = std::min(
            interp_ser,
            std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    install_runtime_parallel_backend();
    Series interp_par_out = time_interp(5);
    double interp_par = 1e300;
    for (int i = 0; i < 5; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = interp_col.interpolate();
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        interp_par = std::min(
            interp_par,
            std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    bool interp_ok = interp_serial_out.length() == interp_par_out.length();
    {
        const double* a2 = interp_serial_out.data<double>();
        const double* b2 = interp_par_out.data<double>();
        for (std::int64_t i = 0; interp_ok && i < interp_serial_out.length();
            ++i) {
            const bool na = interp_serial_out.is_null(i);
            const bool nb = interp_par_out.is_null(i);
            interp_ok = na == nb && (na || a2[i] == b2[i]);
        }
    }
    std::printf("interpolate (null every 32): serial %8.2f ms | runtime "
                "%8.2f ms (%.2fx) correctness: %s\n",
                interp_ser, interp_par, interp_ser / interp_par,
                interp_ok ? "OK" : "MISMATCH");
    ok = ok && interp_ok;

    // dictionary_encode: 5M low-cardinality strings (1261 distinct values).
    auto time_dictenc = [&](int reps) {
        double best = 1e300;
        Series r;
        for (int i = 0; i < reps; ++i) {
            const auto tt0 = std::chrono::steady_clock::now();
            r = str_col.dictionary_encode();
            const auto tt1 = std::chrono::steady_clock::now();
            do_not_optimize(r.length());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(tt1 - tt0).count());
        }
        return r;
    };
    set_parallel_backend(nullptr, nullptr);
    Series de_serial_out = time_dictenc(3);
    double de_ser = 1e300;
    for (int i = 0; i < 3; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = str_col.dictionary_encode();
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        de_ser = std::min(
            de_ser, std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    install_runtime_parallel_backend();
    Series de_par_out = time_dictenc(3);
    double de_par = 1e300;
    for (int i = 0; i < 3; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = str_col.dictionary_encode();
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        de_par = std::min(
            de_par, std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    // Dictionary encoding must match exactly: same first-seen dict order and
    // same per-row codes, so decode both back to plain strings and compare
    // row-by-row (materialize() turns Dictionary back into a FLAT column).
    Series de_serial_mat = de_serial_out.materialize();
    Series de_par_mat = de_par_out.materialize();
    bool de_ok = de_serial_mat.length() == de_par_mat.length();
    for (std::int64_t i = 0; de_ok && i < de_serial_mat.length(); ++i)
        de_ok = de_serial_mat.string_at(i) == de_par_mat.string_at(i);
    std::printf("dictionary_encode (5M, 1261 distinct): serial %8.2f ms | "
                "runtime %8.2f ms (%.2fx) correctness: %s\n",
                de_ser, de_par, de_ser / de_par, de_ok ? "OK" : "MISMATCH");
    ok = ok && de_ok;

    // ewm_mean: affine-transform prefix scan. Random walk data (not a flat
    // series) so the recurrence's decaying tail actually mixes many inputs.
    std::vector<double> ewm_v(static_cast<std::size_t>(rows));
    {
        double acc = 0.0;
        for (std::int64_t i = 0; i < rows; ++i) {
            acc += static_cast<double>(
                       (static_cast<std::uint64_t>(i) * 2654435761ULL) %
                       1000003ULL) /
                   1000003.0 -
                   0.5;
            ewm_v[static_cast<std::size_t>(i)] = acc;
        }
    }
    Series ewm_col = Series::flat(TypeId::Float64, ewm_v.data(), rows);
    const double ewm_alpha = 0.05;
    auto time_ewm = [&](int reps) {
        double best = 1e300;
        Series r;
        for (int i = 0; i < reps; ++i) {
            const auto tt0 = std::chrono::steady_clock::now();
            r = ewm_col.ewm_mean(ewm_alpha);
            const auto tt1 = std::chrono::steady_clock::now();
            do_not_optimize(r.length());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(tt1 - tt0).count());
        }
        return r;
    };
    set_parallel_backend(nullptr, nullptr);
    Series ewm_serial_out = time_ewm(5);
    double ewm_ser = 1e300;
    for (int i = 0; i < 5; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = ewm_col.ewm_mean(ewm_alpha);
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        ewm_ser = std::min(
            ewm_ser, std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    install_runtime_parallel_backend();
    Series ewm_par_out = time_ewm(5);
    double ewm_par = 1e300;
    for (int i = 0; i < 5; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = ewm_col.ewm_mean(ewm_alpha);
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        ewm_par = std::min(
            ewm_par, std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    // Affine composition reassociates the recurrence's rounding, so require a
    // tight relative tolerance rather than bit-identical equality.
    bool ewm_ok = ewm_serial_out.length() == ewm_par_out.length();
    double ewm_max_rel = 0.0;
    {
        const double* a2 = ewm_serial_out.data<double>();
        const double* b2 = ewm_par_out.data<double>();
        for (std::int64_t i = 0; ewm_ok && i < ewm_serial_out.length(); ++i) {
            const double denom = std::max(1e-12, std::fabs(a2[i]));
            const double rel = std::fabs(a2[i] - b2[i]) / denom;
            ewm_max_rel = std::max(ewm_max_rel, rel);
            if (rel > 1e-9) ewm_ok = false;
        }
    }
    std::printf("ewm_mean (alpha=%.2f): serial %8.2f ms | runtime %8.2f ms "
                "(%.2fx) correctness: %s (max rel err %.3e)\n",
                ewm_alpha, ewm_ser, ewm_par, ewm_ser / ewm_par,
                ewm_ok ? "OK" : "MISMATCH", ewm_max_rel);
    ok = ok && ewm_ok;

    // ewm_std: same data/alpha as ewm_mean above.
    auto time_ewm_std = [&](int reps) {
        double best = 1e300;
        Series r;
        for (int i = 0; i < reps; ++i) {
            const auto tt0 = std::chrono::steady_clock::now();
            r = ewm_col.ewm_std(ewm_alpha);
            const auto tt1 = std::chrono::steady_clock::now();
            do_not_optimize(r.length());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(tt1 - tt0).count());
        }
        return r;
    };
    set_parallel_backend(nullptr, nullptr);
    Series es_serial_out = time_ewm_std(5);
    double es_ser = 1e300;
    for (int i = 0; i < 5; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = ewm_col.ewm_std(ewm_alpha);
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        es_ser = std::min(
            es_ser, std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    install_runtime_parallel_backend();
    Series es_par_out = time_ewm_std(5);
    double es_par = 1e300;
    for (int i = 0; i < 5; ++i) {
        const auto tt0 = std::chrono::steady_clock::now();
        Series r = ewm_col.ewm_std(ewm_alpha);
        const auto tt1 = std::chrono::steady_clock::now();
        do_not_optimize(r.length());
        es_par = std::min(
            es_par, std::chrono::duration<double, std::milli>(tt1 - tt0).count());
    }
    bool es_ok = es_serial_out.length() == es_par_out.length();
    double es_max_rel = 0.0;
    std::int64_t es_valid_mismatch = 0;
    {
        const double* a2 = es_serial_out.data<double>();
        const double* b2 = es_par_out.data<double>();
        for (std::int64_t i = 0; es_ok && i < es_serial_out.length(); ++i) {
            const bool na = es_serial_out.is_null(i);
            const bool nb = es_par_out.is_null(i);
            if (na != nb) {
                ++es_valid_mismatch;
                continue;
            }
            if (na) continue;
            const double denom = std::max(1e-12, std::fabs(a2[i]));
            const double rel = std::fabs(a2[i] - b2[i]) / denom;
            es_max_rel = std::max(es_max_rel, rel);
            if (rel > 1e-6) es_ok = false;
        }
        if (es_valid_mismatch > 0) es_ok = false;
    }
    std::printf("ewm_std  (alpha=%.2f): serial %8.2f ms | runtime %8.2f ms "
                "(%.2fx) correctness: %s (max rel err %.3e, valid mismatches "
                "%lld)\n",
                ewm_alpha, es_ser, es_par, es_ser / es_par,
                es_ok ? "OK" : "MISMATCH", es_max_rel,
                static_cast<long long>(es_valid_mismatch));
    ok = ok && es_ok;

    // Lazy optimizer win: sort_by then a selective filter. Eager sorts all
    // rows then filters; lazy reorders the filter ahead of the sort, so the
    // sort runs only on the survivors. Same result (survivors in sorted order),
    // very different cost. Both run with the backend installed.
    install_runtime_parallel_backend();
    const std::int64_t keep_from = rows - rows / 20;  // keep ~5% (v = row index)
    dftu_scalar thr{};
    thr.kind = DFTU_SCALAR_TAG_I64;
    thr.value.i = keep_from;
    Expr keep = expr_cmp(DFTU_CMP_GT, expr_col(1), thr);  // column 1 = v
    auto eager_sort_filter = [&]() {
        DataFrame se = df.sort_by("s");
        Series m = se.column("v") > keep_from;
        return se.filter(m);
    };
    auto lazy_sort_filter = [&]() {
        return run(df.lazy().sort_by("s").filter(keep).collect());
    };
    auto time_it = [&](auto&& fn, int reps) {
        double best = 1e300;
        for (int r = 0; r < reps; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            DataFrame out = fn();
            const auto t1 = std::chrono::steady_clock::now();
            do_not_optimize(out.num_rows());
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        return best;
    };
    const double sf_eager = time_it(eager_sort_filter, 5);
    const double sf_lazy = time_it(lazy_sort_filter, 5);
    DataFrame sf_e = eager_sort_filter();
    DataFrame sf_l = lazy_sort_filter();
    bool sf_ok = sf_e.num_rows() == sf_l.num_rows();
    if (sf_ok) {
        const std::int64_t* es = sf_e.column("s").data<std::int64_t>();
        const std::int64_t* ls = sf_l.column("s").data<std::int64_t>();
        for (std::int64_t i = 0; sf_ok && i < sf_e.num_rows(); ++i)
            sf_ok = es[i] == ls[i];
    }
    std::printf(
        "sort+filter (optimizer reorder): eager %8.2f ms | lazy %8.2f ms "
        "(%.2fx) correctness: %s\n",
        sf_eager, sf_lazy, sf_eager / sf_lazy, sf_ok ? "OK" : "MISMATCH");
    ok = ok && sf_ok;

    // Parity check: a cheap map pipeline where the optimizer has nothing to
    // reorder. Lazy must at least match eager here - if it is far slower, the
    // execution path is copying data it should not. filter (~50%) then a
    // derived column then a projection, over the 3-column df.
    dftu_scalar half{};
    half.kind = DFTU_SCALAR_TAG_I64;
    half.value.i = rows / 2;
    Expr vpos = expr_cmp(DFTU_CMP_GT, expr_col(1), half);       // v > rows/2
    Expr kv = expr_binary(BinaryOp::Add, expr_col(0), expr_col(1));  // k + v
    auto eager_map = [&]() {
        Series m = df.column("v") > (rows / 2);
        DataFrame f = df.filter(m);
        DataFrame w = f.with_column("kv", f.column("k") + f.column("v"));
        return w.select({"k", "kv"});
    };
    auto lazy_map = [&]() {
        return run(df.lazy()
            .filter(vpos)
            .with_column("kv", kv)
            .select({"k", "kv"})
            .collect());
    };
    const double mp_eager = time_it(eager_map, 5);
    const double mp_lazy = time_it(lazy_map, 5);
    DataFrame mp_e = eager_map();
    DataFrame mp_l = lazy_map();
    bool mp_ok = mp_e.num_rows() == mp_l.num_rows() && mp_l.num_columns() == 2;
    if (mp_ok) {
        const std::int64_t* ek = mp_e.column("kv").data<std::int64_t>();
        const std::int64_t* lk = mp_l.column("kv").data<std::int64_t>();
        for (std::int64_t i = 0; mp_ok && i < mp_e.num_rows(); ++i)
            mp_ok = ek[i] == lk[i];
    }
    std::printf(
        "filter+with_column+select (parity): eager %8.2f ms | lazy %8.2f ms "
        "(%.2fx) correctness: %s\n",
        mp_eager, mp_lazy, mp_eager / mp_lazy, mp_ok ? "OK" : "MISMATCH");
    ok = ok && mp_ok;

    return ok ? 0 : 1;
}
