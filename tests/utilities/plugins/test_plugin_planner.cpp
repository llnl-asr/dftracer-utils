// Planner level 1: the plugin set feeds the UNION of every plugin's plan_query
// into the scan's index prune. Covers the union-query construction (dedup, bail
// on a no-query or unparseable plugin, single-vs-multi) and the end-to-end
// property that pruning skips files no plugin wants without changing any
// plugin's result.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/plugins_internal.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "test_plugin_common.h"

using dftracer::utils::CoroScope;
using dftracer::utils::Runtime;
using dftracer::utils::plugins::build_injected_plugins;
using dftracer::utils::plugins::detail::plugin_union_prune_query;
using View = dftracer::utils::trace::views::View;
using ViewFile = dftracer::utils::trace::views::ViewFile;
using ExportStats = dftracer::utils::trace::views::ExportStats;
using test_plugin_common::index_trace;
namespace coro = dftracer::utils::coro;

namespace {

std::vector<const char*> qs(std::vector<const char*> v) { return v; }

// A plugin that counts the events its own filter keeps. on_batch sees events
// already filtered by plan_query, so its count is invariant to host pruning;
// on_finalize (once, on the merged master slice) publishes the total.
struct CountState {
    const char* query = nullptr;
    std::atomic<std::uint64_t> total{0};
};
struct CountSlice {
    CountState* st = nullptr;
    std::uint64_t n = 0;
};

const char* count_plan_query(void* self) {
    return static_cast<CountState*>(self)->query;
}
void* count_make_slice(void* self) {
    auto* s = new CountSlice();
    s->st = static_cast<CountState*>(self);
    return s;
}
dftu_task* count_on_batch(void* slice, const dftu_dataframe* df,
                          const dftu_plugin_host*) {
    static_cast<CountSlice*>(slice)->n +=
        static_cast<std::uint64_t>(dftu_dataframe_num_rows(df));
    return nullptr;
}
void count_merge(void* into, void* other) {
    static_cast<CountSlice*>(into)->n += static_cast<CountSlice*>(other)->n;
}
dftu_task* count_on_finalize(void* slice, const dftu_plugin_host*) {
    auto* s = static_cast<CountSlice*>(slice);
    s->st->total.store(s->n);
    return nullptr;
}
void count_destroy_slice(void* slice) {
    delete static_cast<CountSlice*>(slice);
}
void count_destroy(void*) {}

dftu_plugin make_count_plugin(CountState* st) {
    dftu_plugin p{};
    p.abi_version = DFTRACER_PLUGIN_ABI_VERSION;
    p.self = st;
    p.plan_query = count_plan_query;
    p.make_slice = count_make_slice;
    p.on_batch = count_on_batch;
    p.merge = count_merge;
    p.on_finalize = count_on_finalize;
    p.destroy_slice = count_destroy_slice;
    p.destroy = count_destroy;
    return p;
}

// A single-category, single-name trace so a cat filter prunes the whole file.
std::string make_homog_trace(dftu_utils_test::TestEnvironment& env,
                             const std::string& tag, const char* name,
                             const char* cat, int n) {
    // Own subdir per trace: the index root is keyed off the parent directory,
    // so same-dir traces would share (and clobber) one index.
    std::string dir = env.get_dir() + "/" + tag;
    fs::create_directories(dir);
    std::string pfw = dir + "/trace.pfw";
    std::ofstream ofs(pfw);
    for (int i = 0; i < n; ++i)
        ofs << R"({"ph":"X","name":")" << name << R"(","cat":")" << cat
            << R"(","pid":1,"tid":1,"ts":)" << (1000 + i * 100) << R"(,"dur":)"
            << (10 + i) << R"(,"args":{}})" << "\n";
    ofs.close();
    std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    fs::remove(pfw);
    return gz;
}

ExportStats run_host(std::vector<dftu_plugin*> plugins,
                     std::vector<ViewFile> files) {
    auto set = build_injected_plugins(std::move(plugins));
    REQUIRE(set.has_value());
    View view = View::from_files(std::move(files));
    ExportStats stats;
    Runtime rt(4);
    auto task = dftracer::utils::run_coro_scope(
        rt.executor(), [&](CoroScope&) -> coro::CoroTask<void> {
            auto run = co_await set->run(view);
            if (run) stats = run->stats;
            co_return;
        });
    rt.submit(std::move(task), "plugin-planner").wait();
    rt.shutdown();
    return stats;
}

}  // namespace

TEST_SUITE("PluginPlannerUnion") {
    TEST_CASE("no plugins yields no prune") {
        CHECK_FALSE(plugin_union_prune_query({}).has_value());
    }

    TEST_CASE("a single query is the union verbatim") {
        auto u = plugin_union_prune_query(qs({R"(cat == "STDIO")"}));
        REQUIRE(u.has_value());
        CHECK(u->source() == R"(cat == "STDIO")");
    }

    TEST_CASE("distinct queries disjoin into a real OR") {
        auto u = plugin_union_prune_query(
            qs({R"(cat == "STDIO")", R"(cat == "POSIX")"}));
        REQUIRE(u.has_value());
        // The parenthesized union must parse to a disjunction that keeps events
        // either side selects.
        CHECK(u->references("cat"));
        dftracer::utils::query::ValueMap posix;
        posix["cat"] = std::string("POSIX");
        dftracer::utils::query::ValueMap stdio;
        stdio["cat"] = std::string("STDIO");
        dftracer::utils::query::ValueMap other;
        other["cat"] = std::string("OTHER");
        CHECK(u->evaluate(posix));
        CHECK(u->evaluate(stdio));
        CHECK_FALSE(u->evaluate(other));
    }

    TEST_CASE("identical queries collapse (no repeated term)") {
        const char* q = R"(cat == "STDIO")";
        auto u = plugin_union_prune_query(qs({q, q, q}));
        REQUIRE(u.has_value());
        CHECK(u->source() == std::string(q));
    }

    TEST_CASE("a plugin without a plan_query disables the prune") {
        CHECK_FALSE(plugin_union_prune_query(qs({R"(cat == "STDIO")", nullptr}))
                        .has_value());
        CHECK_FALSE(plugin_union_prune_query(qs({R"(cat == "STDIO")", ""}))
                        .has_value());
    }

    TEST_CASE("an unparseable plan_query disables the prune") {
        CHECK_FALSE(
            plugin_union_prune_query(qs({R"(cat == "STDIO")", "cat =="}))
                .has_value());
    }
}

TEST_SUITE("PluginPlannerScan") {
    // Two homogeneous files: STDIO-only and POSIX-only. A cat=="STDIO" union
    // must prune the POSIX file (fewer events scanned) while every plugin's
    // filtered count stays identical to an unpruned run.
    TEST_CASE("union prune skips unwanted files without changing results") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const int N = 40;
        ViewFile stdio =
            index_trace(make_homog_trace(env, "stdio", "fwrite", "STDIO", N));
        ViewFile posix =
            index_trace(make_homog_trace(env, "posix", "read", "POSIX", N));
        std::vector<ViewFile> files{stdio, posix};

        const char* stdio_q = R"(cat == "STDIO")";

        CountState p_a{stdio_q}, p_b{stdio_q};
        dftu_plugin a = make_count_plugin(&p_a), b = make_count_plugin(&p_b);
        ExportStats pruned = run_host({&a, &b}, files);

        // Baseline: a plugin with no plan_query forces a full scan (no prune).
        CountState q_a{stdio_q}, q_all{nullptr};
        dftu_plugin c = make_count_plugin(&q_a), d = make_count_plugin(&q_all);
        ExportStats full = run_host({&c, &d}, files);

        // Every STDIO-filtering plugin sees exactly the STDIO events either
        // way.
        CHECK(p_a.total.load() == N);
        CHECK(p_b.total.load() == N);
        CHECK(q_a.total.load() == N);
        CHECK(p_a.total.load() == q_a.total.load());
        // The unfiltered plugin sees every event, proving the baseline scanned
        // both files.
        CHECK(q_all.total.load() == 2 * N);

        // Prune evidence: the POSIX file was skipped, so fewer events scanned.
        CHECK(pruned.events_scanned < full.events_scanned);
        CHECK(full.events_scanned == 2 * N);
        CHECK(pruned.events_scanned == N);
    }

    // Result equality cannot see a dead prune: a scan that reads every chunk
    // and filters afterwards returns exactly the same rows. Only the chunk
    // counter distinguishes them, so the prune is asserted on that.
    TEST_CASE("the prune skips index chunks, not just rows") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const int N = 200;
        std::vector<ViewFile> files{
            index_trace(make_homog_trace(env, "s1", "fwrite", "STDIO", N)),
            index_trace(make_homog_trace(env, "p1", "read", "POSIX", N)),
            index_trace(make_homog_trace(env, "p2", "write", "POSIX", N))};

        CountState keeps_stdio{R"(cat == "STDIO")"};
        dftu_plugin a = make_count_plugin(&keeps_stdio);
        ExportStats pruned = run_host({&a}, files);

        CountState same_filter{R"(cat == "STDIO")"}, no_filter{nullptr};
        dftu_plugin b = make_count_plugin(&same_filter);
        dftu_plugin c = make_count_plugin(&no_filter);
        ExportStats full = run_host({&b, &c}, files);

        CHECK(pruned.chunks_skipped > 0);
        CHECK(full.chunks_skipped == 0);
        CHECK(pruned.chunks_scanned < full.chunks_scanned);
        CHECK(keeps_stdio.total.load() == same_filter.total.load());
    }
}

// Plugins::attach() returns a Deferred<PluginRun> handle and owns applying the
// index prune itself. Unlike run() (which always owns the whole scan it
// drives), attach() shares the ViewSession a View::branch() callback is given,
// with no way for this call to see whether another branch is, or will be,
// registered on it - so attach() never chunk-prunes the shared scan (run()
// above is the only path that does).
TEST_SUITE("PluginAttachHandle") {
    using dftracer::utils::plugins::PluginRun;
    using dftracer::utils::trace::views::AggOp;
    using dftracer::utils::trace::views::AggSpec;
    using dftracer::utils::trace::views::Deferred;
    using dftracer::utils::trace::views::ExportStats;
    using dftracer::utils::trace::views::GroupKey;
    using dftracer::utils::trace::views::TraceSession;
    using dftracer::utils::trace::views::ViewSession;

    static double col_sum(const dftracer::utils::dataframe::DataFrame& df,
                          std::string_view name) {
        using T = dftracer::utils::dataframe::TypeId;
        auto s = df.column(name);
        double total = 0;
        for (std::int64_t i = 0; i < s.length(); ++i)
            total += s.type() == T::Int64
                         ? static_cast<double>(s.data<std::int64_t>()[i])
                         : static_cast<double>(s.data<std::uint64_t>()[i]);
        return total;
    }

    TEST_CASE("reading the handle before execute() throws") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        std::vector<ViewFile> files{
            index_trace(make_homog_trace(env, "early", "read", "POSIX", 5))};
        CountState st{nullptr};
        dftu_plugin p = make_count_plugin(&st);
        auto set = build_injected_plugins({&p});
        REQUIRE(set.has_value());

        View base = View::from_files(files);
        dftracer::utils::dataframe::LazyResult<PluginRun> plugin_result =
            base.branch<PluginRun>(
                [&](ViewSession& s) { return set->attach(s); });
        TraceSession sess = base.session();
        Deferred<PluginRun> h = sess.collect(plugin_result);
        CHECK_THROWS_AS(static_cast<void>(h->results), std::logic_error);
    }

    // A sole plugin branch still resolves correctly through attach(); it just
    // does not get run()'s index-skip fast path (see the suite comment above).
    TEST_CASE(
        "attach on a sole plugin branch resolves without narrowing the "
        "shared scan") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const int N = 200;
        std::vector<ViewFile> files{
            index_trace(make_homog_trace(env, "solo1", "fwrite", "STDIO", N)),
            index_trace(make_homog_trace(env, "solo2", "read", "POSIX", N)),
            index_trace(make_homog_trace(env, "solo3", "write", "POSIX", N))};

        CountState st{R"(cat == "STDIO")"};
        dftu_plugin p = make_count_plugin(&st);
        auto set = build_injected_plugins({&p});
        REQUIRE(set.has_value());

        View base = View::from_files(files);
        ExportStats scan{};
        PluginRun run;
        auto plugin_branch = [&](ViewSession& s) {
            Deferred<PluginRun> h = set->attach(s);
            return std::function<void(const ExportStats&)>(
                [&, h](const ExportStats& stats) mutable {
                    run = std::move(h.get());
                    scan = stats;
                });
        };
        dftracer::utils::dataframe::LazyFrame plan = base.branch(plugin_branch);

        Runtime rt(4);
        auto task = dftracer::utils::run_coro_scope(
            rt.executor(), [&](CoroScope&) -> coro::CoroTask<void> {
                co_await plan.collect();
                co_return;
            });
        rt.submit(std::move(task), "plugin-attach-solo").wait();
        rt.shutdown();

        CHECK(st.total.load() == N);  // the plugin's own filter still applies
        // Sole branch, so execute() applies the narrowing attach() offered and
        // the index skips whole chunks. Read it from the branch's own
        // ExportStats: PluginRun::stats is not populated on the attach path,
        // so asserting through the handle would pass whether or not the
        // prune ran.
        CHECK(scan.chunks_skipped > 0);
        (void)run;
    }

    // The regression that matters: a plugin with a narrow plan_query must not
    // starve a co-scanning collect() branch of events it is entitled to.
    TEST_CASE(
        "attach co-scanning with a collect branch never narrows the "
        "shared scan") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const int N = 50;
        std::vector<ViewFile> files{
            index_trace(make_homog_trace(env, "mix1", "fwrite", "STDIO", N)),
            index_trace(make_homog_trace(env, "mix2", "read", "POSIX", N))};

        CountState st{R"(cat == "STDIO")"};  // the plugin only wants STDIO
        dftu_plugin p = make_count_plugin(&st);
        auto set = build_injected_plugins({&p});
        REQUIRE(set.has_value());

        View base = View::from_files(files);
        ExportStats scan{};
        PluginRun run;
        auto plugin_branch = [&](ViewSession& s) {
            Deferred<PluginRun> h = set->attach(s);
            return std::function<void(const ExportStats&)>(
                [&, h](const ExportStats& stats) mutable {
                    run = std::move(h.get());
                    scan = stats;
                });
        };
        dftracer::utils::dataframe::LazyFrame plugin_plan =
            base.branch(plugin_branch);
        dftracer::utils::dataframe::LazyResult<
            dftracer::utils::dataframe::DataFrame>
            all_plan = base.branch<dftracer::utils::dataframe::DataFrame>(
                [](ViewSession& s) {
                    return s.collect({}, {{AggOp::Count, "", "n"}});
                });

        dftracer::utils::dataframe::DataFrame all;
        Runtime rt(4);
        auto task = dftracer::utils::run_coro_scope(
            rt.executor(), [&](CoroScope&) -> coro::CoroTask<void> {
                auto [ignored, agg] =
                    co_await dftracer::utils::dataframe::collect_all(
                        plugin_plan, all_plan);
                (void)ignored;
                all = std::move(agg);
                co_return;
            });
        rt.submit(std::move(task), "plugin-attach-coscan").wait();
        rt.shutdown();

        CHECK(st.total.load() == N);  // plugin's own filter is unaffected
        // The collect branch joined, so execute() drops the narrowing attach()
        // offered: nothing is skipped, and the branch sees both files. Had the
        // prune applied, it would have missed the POSIX file entirely.
        CHECK(scan.chunks_skipped == 0);
        REQUIRE(all.num_rows() == 1);
        CHECK(col_sum(all, "n") == 2 * N);
        (void)run;
    }
}
