#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/trace/agg_query.h>
#include <dftracer/utils/trace/comparator/comparison_aggregation.h>
#include <dftracer/utils/trace/comparator/comparison_collapse.h>
#include <dftracer/utils/trace/comparator/comparison_result.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace dftracer::utils::trace::comparator;
using dftracer::utils::dataframe::FieldStat;
using dftracer::utils::index::schemas::dft::agg::AggPartial;
using dftracer::utils::trace::AggKey;
using dftracer::utils::trace::AggQueryResult;

// `m2` holds the central second moment, sum((x - mean)^2), about the running
// mean (held as `shift`, with `cmean` the remainder); the caller passes both.
static FieldStat make_stats(double mean, double central_m2, uint64_t total,
                            uint64_t min_val, uint64_t max_val,
                            uint64_t count = 0) {
    FieldStat s;
    s.sum = static_cast<double>(total);
    s.min = static_cast<double>(min_val);
    s.max = static_cast<double>(max_val);
    // If count not provided, fall back to total/mean ratio (integer-rounded).
    const double n =
        count > 0 ? static_cast<double>(count)
                  : (mean != 0.0 ? static_cast<double>(total) / mean : 0.0);
    s.n = static_cast<std::uint64_t>(n);
    s.shift = mean;
    s.cm2 = central_m2;
    return s;
}

static dftracer::utils::dstd::StringIntern& test_intern() {
    static dftracer::utils::dstd::StringIntern table;
    return table;
}

// A row of a query grouped by pid and window, with `count` events of equal
// duration and, when `size_total` is set, equal size.
static void add_row(AggQueryResult& result, std::string_view cat,
                    std::string_view name, uint64_t pid, uint64_t window,
                    uint64_t count, uint64_t dur_total, uint64_t size_total) {
    AggKey key;
    key.cat = result.strings().get_or_insert(cat);
    key.name = result.strings().get_or_insert(name);
    key.pid = pid;
    key.window = window;
    AggPartial p;
    p.count = count;
    for (uint64_t i = 0; i < count; ++i) {
        p.duration.add(dur_total / count);
        if (size_total > 0) p.size.add(size_total / count);
    }
    result.rows.emplace(key, std::move(p));
}

static CollapsedMap collapse(const AggQueryResult& windows) {
    AggQueryResult merged;
    return collapse_by_group(windows, merged, ComparisonFilter{},
                             test_intern());
}

static void add_events(CollapsedMetrics& cm, uint64_t count, uint64_t dur) {
    for (uint64_t i = 0; i < count; ++i) {
        ++cm.merged.count;
        cm.merged.duration.add(dur);
    }
}

static CollapsedMetrics make_collapsed(double count_mean, double dur_mean,
                                       double size_mean, double xfer_mean,
                                       double bw_mean, size_t num_windows) {
    CollapsedMetrics cm;
    cm.count_mean = count_mean;
    cm.dur_mean_of_means = dur_mean;
    cm.size_mean_of_means = size_mean;
    cm.xfer_mean = xfer_mean;
    cm.bw_mean = bw_mean;
    cm.num_windows = num_windows;
    add_events(cm, static_cast<size_t>(count_mean),
               static_cast<uint64_t>(dur_mean));
    return cm;
}

TEST_SUITE("ComputeCohensd") {
    TEST_CASE("compute_cohens_d - identical distributions") {
        FieldStat base = make_stats(50.0, 100.0, 500, 10, 100);
        FieldStat var = make_stats(50.0, 100.0, 500, 10, 100);
        double d = compute_cohens_d(base, 10, var, 10);
        CHECK(d == doctest::Approx(0.0).epsilon(1e-10));
    }

    TEST_CASE("compute_cohens_d - large difference") {
        // baseline: mean=10, m2=10 (small variance)
        // variant:  mean=100, m2=10
        FieldStat base = make_stats(10.0, 10.0, 100, 5, 15);
        FieldStat var = make_stats(100.0, 10.0, 1000, 90, 110);
        double d = compute_cohens_d(base, 10, var, 10);
        CHECK(std::abs(d) > 0.8);
    }

    TEST_CASE("compute_cohens_d - insufficient samples") {
        FieldStat base = make_stats(10.0, 50.0, 10, 5, 15);
        FieldStat var = make_stats(20.0, 50.0, 20, 10, 30);

        // n_base = 1 -> returns 0
        CHECK(compute_cohens_d(base, 1, var, 10) ==
              doctest::Approx(0.0).epsilon(1e-10));
        // n_var = 1 -> returns 0
        CHECK(compute_cohens_d(base, 10, var, 1) ==
              doctest::Approx(0.0).epsilon(1e-10));
    }

    TEST_CASE("compute_cohens_d - zero variance") {
        FieldStat base = make_stats(42.0, 0.0, 420, 42, 42);
        FieldStat var = make_stats(42.0, 0.0, 420, 42, 42);
        double d = compute_cohens_d(base, 10, var, 10);
        CHECK(d == doctest::Approx(0.0).epsilon(1e-10));
    }
}

TEST_SUITE("ClassifySignificance") {
    TEST_CASE("classify_significance - negligible") {
        CHECK(classify_significance(0.1) == Significance::Negligible);
    }

    TEST_CASE("classify_significance - small") {
        CHECK(classify_significance(0.3) == Significance::Small);
    }

    TEST_CASE("classify_significance - medium") {
        CHECK(classify_significance(0.6) == Significance::Medium);
    }

    TEST_CASE("classify_significance - large") {
        CHECK(classify_significance(1.0) == Significance::Large);
    }

    TEST_CASE("classify_significance - negative d") {
        // abs(-0.9) = 0.9 > 0.8 -> LARGE
        CHECK(classify_significance(-0.9) == Significance::Large);
    }
}

TEST_SUITE("CollapseByGroup") {
    TEST_CASE("collapse_by_group - single entry") {
        AggQueryResult agg;
        add_row(agg, "POSIX", "lseek64", 1, 0, 5, 500, 0);

        auto result = collapse(agg);
        CHECK(result.size() == 1);
        auto it = result.begin();
        CHECK(it->second.num_windows == 1);
        CHECK(it->second.count_mean == doctest::Approx(5.0));
    }

    TEST_CASE("collapse_by_group - merges time windows") {
        AggQueryResult agg;
        // Same (cat, name, pid=1) across 3 time windows
        add_row(agg, "POSIX", "open", 1, 0, 10, 1000, 0);
        add_row(agg, "POSIX", "open", 1, 1, 20, 2000, 0);
        add_row(agg, "POSIX", "open", 1, 2, 30, 3000, 0);

        auto result = collapse(agg);
        REQUIRE(result.size() == 1);
        const auto& cm = result.begin()->second;
        CHECK(cm.num_windows == 3);
        // mean of per-window max counts: (10+20+30)/3 = 20
        CHECK(cm.count_mean == doctest::Approx(20.0).epsilon(0.01));
    }

    TEST_CASE("collapse_by_group - max across pids") {
        AggQueryResult agg;
        // Same (cat, name, window=0) with different pids
        add_row(agg, "POSIX", "close", 1, 0, 5, 500, 0);
        add_row(agg, "POSIX", "close", 2, 0, 15, 1500, 0);
        add_row(agg, "POSIX", "close", 3, 0, 10, 1000, 0);

        auto result = collapse(agg);
        REQUIRE(result.size() == 1);
        const auto& cm = result.begin()->second;
        // One window (time_bucket=0), max count across pids = 15
        CHECK(cm.num_windows == 1);
        CHECK(cm.count_mean == doctest::Approx(15.0).epsilon(0.01));
    }

    TEST_CASE("collapse_by_group - separate cat/name") {
        AggQueryResult agg;
        add_row(agg, "POSIX", "read", 1, 0, 10, 1000, 4096);
        add_row(agg, "POSIX", "write", 1, 0, 5, 500, 2048);
        add_row(agg, "STDIO", "fread", 1, 0, 3, 300, 1024);

        auto result = collapse(agg);
        CHECK(result.size() == 3);
    }

    TEST_CASE("collapse_by_group - bandwidth only for I/O ops") {
        AggQueryResult agg;
        // read is a data transfer op -> bw_mean > 0
        add_row(agg, "POSIX", "read", 1, 0, 10, 1000, 40960);
        // lseek64 is not -> bw_mean = 0
        add_row(agg, "POSIX", "lseek64", 1, 0, 10, 500, 0);

        auto result = collapse(agg);
        REQUIRE(result.size() == 2);

        for (const auto& [k, cm] : result) {
            if (k.name(test_intern()) == "read") {
                CHECK(cm.bw_mean > 0.0);
            } else {
                CHECK(cm.bw_mean == doctest::Approx(0.0).epsilon(1e-10));
            }
        }
    }
}

TEST_SUITE("ComparisonFilter") {
    static ComparisonFilter filter_of(const std::string& text) {
        auto q = dftracer::utils::duql::Query::from_string(text);
        REQUIRE(q.has_value());
        auto f = filter_from_query(*q);
        REQUIRE_MESSAGE(f.has_value(), text);
        return std::move(*f);
    }

    TEST_CASE("category equality is lower-cased") {
        auto f = filter_of(R"(cat == "POSIX")");
        REQUIRE(f.has_categories);
        REQUIRE(f.categories.size() == 1);
        CHECK(f.categories[0] == "posix");
        CHECK(!f.has_names);
        CHECK(f.keeps("posix", "read"));
        CHECK(!f.keeps("stdio", "read"));
    }

    TEST_CASE("composed node queries intersect") {
        auto f = filter_of(
            R"((cat in ["POSIX", "STDIO"]) AND (name in ["read", "pread"] ))");
        CHECK(f.keeps("posix", "read"));
        CHECK(f.keeps("stdio", "pread"));
        CHECK(!f.keeps("posix", "open"));
        CHECK(!f.keeps("compute", "read"));
        auto g = filter_of(R"((cat == "a") AND (cat == "b"))");
        CHECK(g.keeps_nothing());
    }

    TEST_CASE("or of one field unions") {
        auto f = filter_of(R"(name == "read" or name == "write")");
        CHECK(f.keeps("posix", "read"));
        CHECK(f.keeps("posix", "write"));
        CHECK(!f.keeps("posix", "open"));
    }

    TEST_CASE("pid and ts bounds") {
        auto f = filter_of("pid in [3, 1] AND ts >= 10 AND ts < 20");
        REQUIRE(f.has_pids);
        CHECK(f.pids == dftracer::utils::dstd::Vector<uint64_t>{1, 3});
        CHECK(f.ts_begin == 10);
        CHECK(f.ts_end == 20);
        CHECK(f.same_cut(f));
        CHECK(!f.same_cut(ComparisonFilter{}));
    }

    TEST_CASE("a query the sets cannot say becomes an expression") {
        for (const char* text :
             {R"(name like "%read%")", R"(cat != "posix")",
              R"(not (name == "read"))", R"(cat == "posix" or name == "read")",
              "args.level > 3"}) {
            auto f = filter_of(text);
            CHECK_MESSAGE(f.expr.has_value(), text);
            CHECK(!f.has_categories);
            CHECK(!f.has_names);
            CHECK(!f.keeps_nothing());
        }
        // A set query stays a set query.
        CHECK(!filter_of(R"(cat == "posix")").expr.has_value());
    }

    TEST_CASE("an expression compares the category lower-cased") {
        auto f = filter_of(R"(cat != "POSIX" and name like "%x%")");
        REQUIRE(f.expr.has_value());
        CHECK(f.expr->source().find("lower(cat)") != std::string::npos);
        auto g = filter_of(R"(cat ~ "^PO" or name == "a")");
        REQUIRE(g.expr.has_value());
        CHECK(g.expr->source().find("~*") != std::string::npos);
    }

    TEST_CASE("collapse keeps only the rows of the filter") {
        AggQueryResult agg;
        add_row(agg, "posix", "read", 1, 0, 4, 400, 4096);
        add_row(agg, "posix", "open", 1, 0, 2, 200, 0);
        AggQueryResult merged;
        auto f = filter_of(R"(name == "read")");
        auto result = collapse_by_group(agg, merged, f, test_intern());
        REQUIRE(result.size() == 1);
        CHECK(result.begin()->first.name(test_intern()) == "read");
    }

    TEST_CASE("collapse attaches the merged stats and quantiles") {
        AggQueryResult agg;
        add_row(agg, "posix", "read", 1, 0, 4, 400, 0);
        AggQueryResult merged;
        AggKey key;
        key.cat = merged.strings().get_or_insert("posix");
        key.name = merged.strings().get_or_insert("read");
        AggPartial p;
        for (uint64_t v : {10, 20, 30, 40, 50}) {
            ++p.count;
            p.duration.add(v);
            p.duration_q.add(static_cast<double>(v));
        }
        merged.rows.emplace(key, std::move(p));
        auto result =
            collapse_by_group(agg, merged, ComparisonFilter{}, test_intern());
        REQUIRE(result.size() == 1);
        const auto& cm = result.begin()->second;
        CHECK(cm.merged.count == 5);
        CHECK(cm.merged.duration_q.quantile(0.5) == doctest::Approx(30.0));
    }

    TEST_CASE("metadata counts processes, threads and file pairs") {
        AggQueryResult merged;
        auto row = [&](std::string_view name, uint64_t ts, uint64_t te,
                       uint64_t dur_total, auto&& observe) {
            AggKey key;
            key.cat = merged.strings().get_or_insert("posix");
            key.name = merged.strings().get_or_insert(name);
            AggPartial p;
            p.count = 1;
            p.ts = ts;
            p.te = te;
            p.duration.add(dur_total);
            p.size.add(uint64_t{100});
            observe(p);
            merged.rows.emplace(key, std::move(p));
        };
        // Thread 3 of pid 2 and pid 1 are two threads; pid 2 touches file 8
        // twice, across both names, which is one pair.
        row("read", 90, 400, 310, [](AggPartial& p) {
            p.observe(1, 1, 7);
            p.observe(1, 2, 7);
            p.observe(2, 3, 8);
        });
        row("write", 95, 96, 1, [](AggPartial& p) {
            p.observe(2, 3, 8);
            p.observe(2, 3, 0);
        });
        auto meta = extract_metadata(merged, ComparisonFilter{}, 9);
        CHECK(meta.process_count == 2);
        CHECK(meta.thread_count == 3);
        CHECK(meta.file_count == 2);
        CHECK(meta.proc_file_count == 2);
        CHECK(meta.makespan_us == doctest::Approx(310.0));
        CHECK(meta.total_bytes == doctest::Approx(200.0));
        CHECK(meta.total_io_time_us == doctest::Approx(311.0));
    }
}

TEST_SUITE("CompareMetrics") {
    TEST_CASE("compare_metrics - count metric") {
        auto base = make_collapsed(100.0, 500.0, 0.0, 0.0, 0.0, 1);
        auto var = make_collapsed(150.0, 500.0, 0.0, 0.0, 0.0, 1);

        auto results = compare_metrics(base, var, {"count"}, {});
        REQUIRE(results.size() == 1);
        const auto& cmp = results[0];
        CHECK(cmp.metric_name == "count");
        CHECK(cmp.delta == doctest::Approx(50.0).epsilon(0.01));
        CHECK(cmp.pct_change == doctest::Approx(50.0).epsilon(0.01));
    }

    TEST_CASE("compare_metrics - duration metric") {
        auto base = make_collapsed(10.0, 200.0, 0.0, 0.0, 0.0, 1);
        auto var = make_collapsed(10.0, 300.0, 0.0, 0.0, 0.0, 1);

        auto results = compare_metrics(base, var, {"duration"}, {});
        // At least the mean entry; no percentile entries since percentiles={}
        REQUIRE_FALSE(results.empty());
        auto it = std::find_if(results.begin(), results.end(),
                               [](const MetricComparison& c) {
                                   return c.metric_name == "dur_mean";
                               });
        REQUIRE(it != results.end());
        CHECK(it->baseline_value == doctest::Approx(200.0).epsilon(0.01));
        CHECK(it->variant_value == doctest::Approx(300.0).epsilon(0.01));
    }

    TEST_CASE("compare_metrics - size metric") {
        auto base = make_collapsed(10.0, 100.0, 512.0, 0.0, 0.0, 1);
        auto var = make_collapsed(10.0, 100.0, 1024.0, 0.0, 0.0, 1);

        auto results = compare_metrics(base, var, {"size"}, {});
        REQUIRE_FALSE(results.empty());
        auto it = std::find_if(results.begin(), results.end(),
                               [](const MetricComparison& c) {
                                   return c.metric_name == "size_mean";
                               });
        REQUIRE(it != results.end());
        CHECK(it->baseline_value == doctest::Approx(512.0).epsilon(0.01));
        CHECK(it->variant_value == doctest::Approx(1024.0).epsilon(0.01));
    }

    TEST_CASE("compare_metrics - transfer_size skipped when zero") {
        auto base = make_collapsed(10.0, 100.0, 0.0, 0.0, 0.0, 1);
        auto var = make_collapsed(10.0, 100.0, 0.0, 0.0, 0.0, 1);

        auto results = compare_metrics(base, var, {"transfer_size"}, {});
        auto it = std::find_if(results.begin(), results.end(),
                               [](const MetricComparison& c) {
                                   return c.metric_name == "transfer_size";
                               });
        CHECK(it == results.end());
    }

    TEST_CASE("compare_metrics - bandwidth regression is decrease") {
        // Higher baseline bw -> variant is slower -> regression
        auto base = make_collapsed(10.0, 100.0, 0.0, 0.0, 1000.0, 1);
        auto var = make_collapsed(10.0, 100.0, 0.0, 0.0, 500.0, 1);

        auto results = compare_metrics(base, var, {"bandwidth"}, {});
        REQUIRE(results.size() == 1);
        CHECK(results[0].metric_name == "bandwidth");
        CHECK(results[0].is_regression == true);
    }

    TEST_CASE("compare_metrics - zero baseline pct_change") {
        // baseline count = 0 (merged has no updates), variant count = 100
        CollapsedMetrics base;
        base.count_mean = 0.0;
        base.num_windows = 1;

        CollapsedMetrics var;
        var.count_mean = 100.0;
        var.num_windows = 1;
        add_events(var, 100, 10);

        auto results = compare_metrics(base, var, {"count"}, {});
        REQUIRE(results.size() == 1);
        CHECK(results[0].pct_change == doctest::Approx(100.0).epsilon(0.01));
    }
}
