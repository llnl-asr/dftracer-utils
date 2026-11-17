// SPDX-License-Identifier: MIT
// Seeded phase results (dftu_svc_ports::phase_seed): the host fills and seals a
// seeded generation before the plugin scan, from the index statistics, from a
// cache keyed by the files' stat and sample hash, or from a pass of its own.
// Every value is checked against a reference computed here from the events the
// test wrote, and the plan rules and the refusal to contribute are checked.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/plugins/abi.h>
#include <dftracer/utils/plugins/host/phase_seed.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/plugins/host/plugins_internal.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/build/resolve_and_build.h>
#include <dftracer/utils/trace/index/cache/phase_seed_store.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <doctest/doctest.h>
#include <plugins/test_plugin_common.h>
#include <testing_utilities.h>

#include <atomic>
#include <cstdint>
#include <limits>

using namespace dftracer::utils;
using plugins::host::InjectedPlugin;
using plugins::host::SeedSource;
using trace::views::View;
using trace::views::ViewFile;

namespace {

struct TestEvent {
    std::uint64_t ts;
    std::uint64_t dur;
    bool metadata = false;
};

struct Reference {
    std::uint64_t count = 0;
    std::uint64_t ts_min = 0;
    std::uint64_t ts_max = 0;
};

// The three seeds as the files' events define them: every event that is not
// metadata, the earliest start above 0, the latest end.
Reference reference_of(const dstd::Vector<dstd::Vector<TestEvent>>& files) {
    Reference ref;
    bool any_start = false;
    for (const auto& events : files) {
        for (const TestEvent& e : events) {
            if (e.metadata) continue;
            ++ref.count;
            if (e.ts > 0 && (!any_start || e.ts < ref.ts_min)) {
                ref.ts_min = e.ts;
                any_start = true;
            }
            if (e.ts + e.dur > ref.ts_max) ref.ts_max = e.ts + e.dur;
        }
    }
    return ref;
}

// Indexes `gz_files` whole, statistics included, as a View on them would
// rebuild a stale index.
dstd::Vector<ViewFile> build_indexes(
    const dstd::Vector<dstd::String>& gz_files) {
    trace::index::build::ResolveAndBuildInput in;
    for (const auto& gz : gz_files) in.files.emplace_back(gz);
    in.require_checkpoints = true;
    Runtime rt(2);
    auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
        dftu_utils_test::ok(
            co_await trace::index::build::resolve_and_build_index(
                std::move(in)));
        co_return;
    });
    dftu_utils_test::ok(rt.submit(std::move(task), "phase-seed-index").wait());
    rt.shutdown();
    dstd::Vector<ViewFile> files;
    for (const auto& gz : gz_files) {
        files.push_back(ViewFile{
            gz, dstd::String(trace::internal::determine_index_path(gz, ""))});
    }
    return files;
}

dstd::String write_trace(dftu_utils_test::TestEnvironment& env,
                         const char* name,
                         const dstd::Vector<TestEvent>& events) {
    const dstd::String dir = env.dir() + "/seed";
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String pfw = dir + "/" + name + ".pfw";
    dstd::String lines;
    for (const TestEvent& e : events) {
        if (e.metadata) {
            lines +=
                "{\"ph\":\"M\",\"name\":\"process_name\",\"cat\":\"dftracer\","
                "\"pid\":1,\"tid\":1,\"args\":{\"name\":\"app\"}}\n";
            continue;
        }
        dstd::fmt_append<
            "{\"ph\":\"X\",\"name\":\"read\",\"cat\":\"POSIX\",\"pid\":1,"
            "\"tid\":1,\"ts\":{},\"dur\":{}{}\n">(lines, e.ts, e.dur,
                                                  R"(,"args":{}})");
    }
    dstd::fs::write_file(dstd::PathView(pfw), lines).discard();
    const dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dstd::fs::remove(pfw).discard();
    return gz;
}

dstd::String write_raw_trace(dftu_utils_test::TestEnvironment& env,
                             const char* folder, const char* name,
                             const dstd::String& lines) {
    const dstd::String dir = env.dir() + "/" + folder;
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String pfw = dir + "/" + name + ".pfw";
    dstd::fs::write_file(dstd::PathView(pfw), lines).discard();
    const dstd::String gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    dstd::fs::remove(pfw).discard();
    return gz;
}

constexpr std::uint64_t UNSET = std::numeric_limits<std::uint64_t>::max();

// What the plugin read from the phase results in its on_batch.
struct Seen {
    std::atomic<std::uint64_t> start{UNSET};
    std::atomic<std::uint64_t> end{UNSET};
    std::atomic<std::uint64_t> count{UNSET};
    std::atomic<int> not_ready{0};
    std::atomic<int> batches{0};

    void reset() {
        start = end = count = UNSET;
        not_ready = batches = 0;
    }
};
Seen g_seen;
bool g_contribute = false;

dftu_status declare_seeds(dftu_ctx* ctx, const dftu_plugin_host* host) {
    plugins::Ports ports(host, ctx);
    struct Spec {
        const char* name;
        plugins::MergeOp op;
        plugins::PhaseSeed seed;
    };
    const Spec specs[] = {
        {"start", plugins::MergeOp::Min, plugins::PhaseSeed::TsMin},
        {"end", plugins::MergeOp::Max, plugins::PhaseSeed::TsMax},
        {"count", plugins::MergeOp::Sum, plugins::PhaseSeed::EventCount}};
    for (const Spec& s : specs) {
        auto gen = ports.phase_declare(s.name, s.op, plugins::PhaseType::U64);
        if (!gen) return plugins::status_of(gen.error());
        auto seeded = ports.seed(s.name, *gen, s.seed);
        if (!seeded) return plugins::status_of(seeded.error());
    }
    return DFTU_OK;
}

void read_seed(const plugins::Ports& ports, const char* name,
               std::atomic<std::uint64_t>& slot) {
    std::uint64_t value = 0;
    auto got = ports.phase_result(
        name, 1,
        dstd::MutableByteView(reinterpret_cast<std::byte*>(&value),
                              sizeof value));
    if (got) {
        slot.store(value);
    } else {
        ++g_seen.not_ready;
    }
}

dftu_status read_seeds_on_batch(dftu_ctx* ctx, void*, const dftu_dataframe*,
                                const dftu_plugin_host* host) {
    plugins::Ports ports(host, ctx);
    ++g_seen.batches;
    read_seed(ports, "start", g_seen.start);
    read_seed(ports, "end", g_seen.end);
    read_seed(ports, "count", g_seen.count);
    if (g_contribute) {
        const std::uint64_t one = 1;
        auto refused = ports.phase_contribute(
            "count", 1, plugins::MergeOp::Sum, plugins::PhaseType::U64,
            dstd::ByteView(reinterpret_cast<const std::byte*>(&one),
                           sizeof one));
        if (!refused) return plugins::status_of(refused.error());
    }
    return DFTU_OK;
}

dftu_status make_empty_slice(dftu_ctx*, void*, void** out) {
    *out = new int(0);
    return DFTU_OK;
}
void destroy_empty_slice(void* slice) { delete static_cast<int*>(slice); }
dftu_status no_merge(dftu_ctx*, void*, void*) { return DFTU_OK; }
void no_destroy(void*) {}
dftu_status no_finalize(dftu_ctx*, void*, const dftu_plugin_host*,
                        dftu_task**) {
    return DFTU_OK;
}

dftu_plugin make_seed_plugin() {
    dftu_plugin p{};
    p.struct_size = sizeof(p);
    p.make_slice = make_empty_slice;
    p.merge = no_merge;
    p.on_finalize = no_finalize;
    p.destroy_slice = destroy_empty_slice;
    p.destroy = no_destroy;
    p.on_batch = read_seeds_on_batch;
    return p;
}

dstd::Result<plugins::host::Plugins> seed_set(dftu_plugin* plugin) {
    return plugins::host::build_injected_plugins_declaring(
        {InjectedPlugin{plugin, "seeder", declare_seeds}});
}

dstd::Result<plugins::host::PluginRun> run_view(
    const plugins::host::Plugins& set, const View& view) {
    dstd::Optional<dstd::Result<plugins::host::PluginRun>> out;
    Runtime rt(2);
    auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
        auto op = set.run(view);
        out.emplace(co_await op);
        co_return;
    });
    dftu_utils_test::ok(rt.submit(std::move(task), "phase-seed-run").wait());
    rt.shutdown();
    REQUIRE(out.has_value());
    return std::move(*out);
}

void check_seen(const Reference& ref) {
    CHECK(g_seen.batches.load() > 0);
    CHECK(g_seen.not_ready.load() == 0);
    CHECK(g_seen.start.load() == ref.ts_min);
    CHECK(g_seen.end.load() == ref.ts_max);
    CHECK(g_seen.count.load() == ref.count);
}

bool has_text(const dstd::String& haystack, const char* needle) {
    return haystack.find(needle) != dstd::String::npos;
}

// Functions, since a global dstd::Vector would hold its block past the memory
// exit check.
dstd::Vector<TestEvent> first_events() {
    return {{5000, 40},   {1200, 8},   {0, 3},
            {0, 0, true}, {9100, 900}, {3000, 20}};
}
dstd::Vector<TestEvent> second_events() {
    return {{700, 5}, {20000, 15}, {0, 0, true}, {650, 1}};
}

}  // namespace

TEST_SUITE("phase seeds: sources") {
    TEST_CASE("a current index seeds the exact reference values") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const ViewFile file =
            build_indexes({write_trace(env, "one", first_events())})[0];
        dftu_plugin plugin = make_seed_plugin();
        auto set = seed_set(&plugin);
        REQUIRE(set.has_value());

        g_seen.reset();
        g_contribute = false;
        auto run = run_view(*set, View::from_files({file}));
        REQUIRE(run.has_value());
        CHECK(run->seed_source == SeedSource::Index);
        check_seen(reference_of({first_events()}));
        CHECK(g_seen.start.load() == 1200);
        CHECK(g_seen.end.load() == 10000);
        CHECK(g_seen.count.load() == 5);
    }

    TEST_CASE("the index merge covers every file of the run") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const dstd::Vector<ViewFile> files =
            build_indexes({write_trace(env, "a", first_events()),
                           write_trace(env, "b", second_events())});
        dftu_plugin plugin = make_seed_plugin();
        auto set = seed_set(&plugin);
        REQUIRE(set.has_value());

        g_seen.reset();
        g_contribute = false;
        auto run = run_view(*set, View::from_files(files));
        REQUIRE(run.has_value());
        CHECK(run->seed_source == SeedSource::Index);
        check_seen(reference_of({first_events(), second_events()}));
        CHECK(g_seen.start.load() == 650);
        CHECK(g_seen.end.load() == 20015);
    }

    TEST_CASE("a filter on the view does not narrow the seeds") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const ViewFile file =
            build_indexes({write_trace(env, "filtered", first_events())})[0];
        dftu_plugin plugin = make_seed_plugin();
        auto set = seed_set(&plugin);
        REQUIRE(set.has_value());

        g_seen.reset();
        g_contribute = false;
        auto run =
            run_view(*set, View::from_files({file}).filter(
                               duql::Query::from_string("ts > 4000").value()));
        REQUIRE(run.has_value());
        check_seen(reference_of({first_events()}));
    }

    TEST_CASE(
        "a file with no index is scanned once, and the next run takes the "
        "values from the cache") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const dstd::String gz = write_trace(env, "plain", first_events());
        dftu_plugin plugin = make_seed_plugin();
        auto set = seed_set(&plugin);
        REQUIRE(set.has_value());
        const Reference ref = reference_of({first_events()});

        auto first_scans = dstd::make_shared<trace::views::ScanCounter>();
        g_seen.reset();
        g_contribute = false;
        auto first =
            run_view(*set, View::from_file(gz).count_scans(first_scans));
        REQUIRE(first.has_value());
        CHECK(first->seed_source == SeedSource::Pass);
        check_seen(ref);

        auto second_scans = dstd::make_shared<trace::views::ScanCounter>();
        g_seen.reset();
        auto second =
            run_view(*set, View::from_file(gz).count_scans(second_scans));
        REQUIRE(second.has_value());
        CHECK(second->seed_source == SeedSource::Cache);
        check_seen(ref);
        // The first run read the file twice, once for the seeds; the second
        // only for the plugin.
        CHECK(first_scans->events_scanned.load() ==
              2 * second_scans->events_scanned.load());
    }

    TEST_CASE("a file that changed after its index never gets old values") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const ViewFile file =
            build_indexes({write_trace(env, "changing", first_events())})[0];
        dftu_plugin plugin = make_seed_plugin();
        auto set = seed_set(&plugin);
        REQUIRE(set.has_value());

        g_seen.reset();
        g_contribute = false;
        auto before = run_view(*set, View::from_files({file}));
        REQUIRE(before.has_value());
        CHECK(before->seed_source == SeedSource::Index);
        check_seen(reference_of({first_events()}));

        // The view is made while the index is current, then a line is
        // appended: the index no longer matches the file.
        const View view = View::from_files({file});
        dstd::Vector<TestEvent> grown = first_events();
        grown.push_back({50000, 77});
        write_trace(env, "changing", grown);

        g_seen.reset();
        auto after = run_view(*set, view);
        REQUIRE(after.has_value());
        CHECK(after->seed_source == SeedSource::Pass);
        check_seen(reference_of({grown}));
        CHECK(g_seen.end.load() == 50077);
    }

    TEST_CASE("a file rewritten after a run never gets the old values") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const dstd::String gz = write_trace(env, "keyed", first_events());
        dftu_plugin plugin = make_seed_plugin();
        auto set = seed_set(&plugin);
        REQUIRE(set.has_value());

        g_seen.reset();
        g_contribute = false;
        auto first = run_view(*set, View::from_file(gz));
        REQUIRE(first.has_value());
        CHECK(first->seed_source == SeedSource::Pass);
        check_seen(reference_of({first_events()}));

        dstd::Vector<TestEvent> grown = first_events();
        grown.push_back({400, 2});
        write_trace(env, "keyed", grown);

        g_seen.reset();
        auto second = run_view(*set, View::from_file(gz));
        REQUIRE(second.has_value());
        CHECK(second->seed_source != SeedSource::Cache);
        check_seen(reference_of({grown}));
        CHECK(g_seen.start.load() == 400);
    }

    TEST_CASE("a plan with no seed runs no seed work") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const ViewFile file =
            build_indexes({write_trace(env, "none", first_events())})[0];
        dftu_plugin plugin = make_seed_plugin();
        plugin.on_batch = nullptr;
        auto set = plugins::host::build_injected_plugins({&plugin});
        REQUIRE(set.has_value());

        auto run = run_view(*set, View::from_files({file}));
        REQUIRE(run.has_value());
        CHECK(run->seed_source == SeedSource::None);
    }
}

TEST_SUITE("phase seeds: plugins") {
    TEST_CASE("a plugin cannot contribute to a seeded generation") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const ViewFile file =
            build_indexes({write_trace(env, "refused", first_events())})[0];
        dftu_plugin plugin = make_seed_plugin();
        auto set = seed_set(&plugin);
        REQUIRE(set.has_value());

        g_seen.reset();
        g_contribute = true;
        auto run = run_view(*set, View::from_files({file}));
        g_contribute = false;
        REQUIRE_FALSE(run.has_value());
        const dstd::String message(run.error().message());
        CHECK(has_text(message, "plugin.seeder.count"));
        CHECK(has_text(message, "seeded by the host"));
        // The refused contribution did not change the sealed value.
        CHECK(g_seen.count.load() == 5);
    }

    TEST_CASE("an attached set seeds before its first batch too") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const ViewFile file =
            build_indexes({write_trace(env, "attached", first_events())})[0];
        dftu_plugin plugin = make_seed_plugin();
        auto set = seed_set(&plugin);
        REQUIRE(set.has_value());

        g_seen.reset();
        g_contribute = false;
        const View base = View::from_files({file});
        SeedSource source = SeedSource::None;
        auto branch = [&](trace::views::ViewSession& s) {
            trace::views::Deferred<plugins::host::PluginRun> h = set->attach(s);
            return dstd::Function<void(const trace::views::ExportStats&)>(
                [&source, h](const trace::views::ExportStats&) mutable {
                    source = h.get().seed_source;
                });
        };
        dataframe::LazyFrame plan = base.branch(branch);
        Runtime rt(4);
        auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
            dftu_utils_test::ok(co_await plan.collect());
            co_return;
        });
        dftu_utils_test::ok(
            rt.submit(std::move(task), "phase-seed-attach").wait());
        rt.shutdown();

        CHECK(source == SeedSource::Index);
        check_seen(reference_of({first_events()}));
    }

    TEST_CASE("an attached set over an unindexed file seeds from a pass") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const dstd::String gz =
            write_trace(env, "attached_plain", first_events());
        dftu_plugin plugin = make_seed_plugin();
        auto set = seed_set(&plugin);
        REQUIRE(set.has_value());

        g_seen.reset();
        g_contribute = false;
        const View base = View::from_file(gz);
        SeedSource source = SeedSource::None;
        auto branch = [&](trace::views::ViewSession& s) {
            trace::views::Deferred<plugins::host::PluginRun> h = set->attach(s);
            return dstd::Function<void(const trace::views::ExportStats&)>(
                [&source, h](const trace::views::ExportStats&) mutable {
                    source = h.get().seed_source;
                });
        };
        dataframe::LazyFrame plan = base.branch(branch);
        Runtime rt(4);
        auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
            dftu_utils_test::ok(co_await plan.collect());
            co_return;
        });
        dftu_utils_test::ok(
            rt.submit(std::move(task), "phase-seed-attach-plain").wait());
        rt.shutdown();

        CHECK(source != SeedSource::None);
        check_seen(reference_of({first_events()}));
    }
}

namespace {

// Records of every phase letter, one with no "ph" and one of metadata, among
// enough ordinary events for the file to be read as dftracer events.
dstd::String mixed_phase_lines() {
    dstd::String lines;
    for (int i = 0; i < 30; ++i) {
        dstd::fmt_append<
            "{\"ph\":\"X\",\"name\":\"read\",\"cat\":\"POSIX\",\"pid\":1,"
            "\"tid\":1,\"ts\":{},\"dur\":2{}\n">(lines, 1000 + i,
                                                 R"(,"args":{}})");
    }
    return lines +
           "{\"ph\":\"B\",\"name\":\"b\",\"cat\":\"c\",\"pid\":1,"
           "\"tid\":1,\"ts\":100,\"dur\":5,\"args\":{}}\n"
           "{\"ph\":\"i\",\"name\":\"i\",\"cat\":\"c\",\"pid\":1,"
           "\"tid\":1,\"ts\":40,\"args\":{}}\n"
           "{\"name\":\"noph\",\"cat\":\"c\",\"pid\":1,\"tid\":1,"
           "\"ts\":10,\"dur\":5,\"args\":{}}\n"
           "{\"ph\":\"M\",\"name\":\"process_name\",\"cat\":\"dftracer\","
           "\"pid\":1,\"tid\":1,\"args\":{\"name\":\"app\"}}\n"
           "{\"ph\":\"X\",\"name\":\"x\",\"cat\":\"c\",\"pid\":1,"
           "\"tid\":1,\"ts\":300,\"dur\":20,\"args\":{}}\n";
}

// The rule of counts_as_event: a record with a "ph" other than metadata, of any
// letter; a record with no "ph" is not an event.
Reference mixed_phase_reference() {
    Reference ref;
    ref.count = 33;
    ref.ts_min = 40;
    ref.ts_max = 1031;
    return ref;
}

}  // namespace

TEST_SUITE("phase seeds: records") {
    TEST_CASE("every route counts the records that count_as_event counts") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const Reference ref = mixed_phase_reference();
        dftu_plugin plugin = make_seed_plugin();
        auto set = seed_set(&plugin);
        REQUIRE(set.has_value());
        g_contribute = false;

        const dstd::String indexed_gz =
            write_raw_trace(env, "raw_a", "indexed", mixed_phase_lines());
        const dstd::Vector<ViewFile> indexed = build_indexes({indexed_gz});
        g_seen.reset();
        auto by_index = run_view(*set, View::from_files(indexed));
        REQUIRE(by_index.has_value());
        CHECK(by_index->seed_source == SeedSource::Index);
        check_seen(ref);

        const dstd::String plain_gz =
            write_raw_trace(env, "raw_b", "plain", mixed_phase_lines());
        g_seen.reset();
        auto by_pass = run_view(*set, View::from_file(plain_gz));
        REQUIRE(by_pass.has_value());
        CHECK(by_pass->seed_source == SeedSource::Pass);
        check_seen(ref);

        g_seen.reset();
        auto by_cache = run_view(*set, View::from_file(plain_gz));
        REQUIRE(by_cache.has_value());
        CHECK(by_cache->seed_source == SeedSource::Cache);
        check_seen(ref);
    }

    TEST_CASE("a file read by path gets no dftracer seeds from any route") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        dftu_plugin plugin = make_seed_plugin();
        auto set = seed_set(&plugin);
        REQUIRE(set.has_value());
        g_contribute = false;
        // Too few ordinary events for the file to be read as dftracer events.
        const dstd::String lines =
            "{\"a\":1,\"b\":\"x\"}\n{\"a\":2,\"b\":\"y\"}\n";
        const dstd::Vector<ViewFile> files =
            build_indexes({write_raw_trace(env, "raw_c", "generic", lines)});
        auto run = run_view(*set, View::from_files(files));
        REQUIRE_FALSE(run.has_value());
        CHECK(has_text(dstd::String(run.error().message()), "by path"));
    }
}

#if DFTRACER_UTILS_TEST_PROBES
namespace {

// What the pass hook does to the file: 0 nothing, 1 append once, 2 append on
// every attempt.
int g_hook_mode = 0;
dftu_utils_test::TestEnvironment* g_hook_env = nullptr;
dstd::String* g_hook_name = nullptr;

void change_file_in_pass(int attempt) {
    if (g_hook_mode == 0 || (g_hook_mode == 1 && attempt > 0)) return;
    dstd::Vector<TestEvent> grown = first_events();
    for (int i = 0; i <= attempt; ++i) {
        grown.push_back({60000 + static_cast<std::uint64_t>(i), 1});
    }
    write_trace(*g_hook_env, g_hook_name->c_str(), grown);
}

}  // namespace

TEST_SUITE("phase seeds: files that change during the pass") {
    TEST_CASE(
        "a file that changes once is read again and cached under its key") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        dstd::String name = "moving";
        const dstd::String gz = write_trace(env, "moving", first_events());
        dftu_plugin plugin = make_seed_plugin();
        auto set = seed_set(&plugin);
        REQUIRE(set.has_value());
        g_contribute = false;

        const auto old_plan = trace::views::detail::scan::from_file(
            gz, dstd::String(trace::internal::determine_index_path(gz, "")));
        const auto old_key = trace::index::cache::input_fingerprint(*old_plan);
        const dstd::String cache_path =
            trace::index::cache::phase_seed_cache_path(*old_plan);
        REQUIRE(old_key.has_value());
        REQUIRE_FALSE(cache_path.empty());

        plugins::host::g_phase_seed_pass_hook = change_file_in_pass;
        g_hook_mode = 1;
        g_hook_env = &env;
        g_hook_name = &name;
        g_seen.reset();
        auto run = run_view(*set, View::from_file(gz));
        g_hook_mode = 0;
        plugins::host::g_phase_seed_pass_hook = nullptr;
        REQUIRE(run.has_value());
        CHECK(run->seed_source == SeedSource::Pass);
        dstd::Vector<TestEvent> grown = first_events();
        grown.push_back({60000, 1});
        // The values are those of the file as it ended, never a mix.
        check_seen(reference_of({grown}));

        auto db = trace::index::cache::CacheStore::open(
            cache_path, trace::index::cache::CacheStore::Mode::ReadOnly);
        REQUIRE(db);
        auto old_entry = trace::index::cache::read_input_stats(*db, *old_key);
        REQUIRE(old_entry.has_value());
        CHECK_FALSE(old_entry->has_value());
        const auto new_plan = trace::views::detail::scan::from_file(
            gz, dstd::String(trace::internal::determine_index_path(gz, "")));
        const auto new_key = trace::index::cache::input_fingerprint(*new_plan);
        REQUIRE(new_key.has_value());
        auto new_entry = trace::index::cache::read_input_stats(*db, *new_key);
        REQUIRE(new_entry.has_value());
        REQUIRE(new_entry->has_value());
        CHECK((*new_entry)->event_count == reference_of({grown}).count);
    }

    TEST_CASE("a file that keeps changing fails the run, naming the file") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        dstd::String name = "restless";
        const dstd::String gz = write_trace(env, "restless", first_events());
        dftu_plugin plugin = make_seed_plugin();
        auto set = seed_set(&plugin);
        REQUIRE(set.has_value());
        g_contribute = false;

        const auto plan = trace::views::detail::scan::from_file(
            gz, dstd::String(trace::internal::determine_index_path(gz, "")));
        const dstd::String cache_path =
            trace::index::cache::phase_seed_cache_path(*plan);

        plugins::host::g_phase_seed_pass_hook = change_file_in_pass;
        g_hook_mode = 2;
        g_hook_env = &env;
        g_hook_name = &name;
        auto run = run_view(*set, View::from_file(gz));
        g_hook_mode = 0;
        plugins::host::g_phase_seed_pass_hook = nullptr;
        REQUIRE_FALSE(run.has_value());
        const dstd::String message(run.error().message());
        CHECK(has_text(message, "restless.pfw.gz"));
        CHECK(has_text(message, "changed while the host read it"));
        // No value of a moving file was stored under any key.
        if (trace::index::cache::CacheStore::exists(cache_path)) {
            auto db = trace::index::cache::CacheStore::open(
                cache_path, trace::index::cache::CacheStore::Mode::ReadOnly);
            REQUIRE(db);
            auto usage = db->usage();
            REQUIRE(usage.has_value());
            CHECK(usage->empty());
        }
    }
}
#endif

namespace {

// Every file under `dir` with its size, as text, to compare two states.
dstd::String tree_listing(const dstd::String& dir) {
    dstd::String out;
    for (const auto& e : dftu_utils_test::walk_dir(dstd::PathView(dir))) {
        out += e.path.c_str();
        out += ' ';
        out += dstd::to_string(e.size);
        out += '\n';
    }
    return out;
}

}  // namespace

TEST_SUITE("phase seeds: the cache") {
    TEST_CASE("a cache hit writes nothing to the cache store") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const dstd::String gz = write_trace(env, "readonly", first_events());
        dftu_plugin plugin = make_seed_plugin();
        auto set = seed_set(&plugin);
        REQUIRE(set.has_value());
        g_contribute = false;

        const auto plan = trace::views::detail::scan::from_file(
            gz, dstd::String(trace::internal::determine_index_path(gz, "")));
        const dstd::String cache_path =
            trace::index::cache::phase_seed_cache_path(*plan);
        REQUIRE_FALSE(cache_path.empty());

        auto first = run_view(*set, View::from_file(gz));
        REQUIRE(first.has_value());
        CHECK(first->seed_source == SeedSource::Pass);
        const dstd::String before = tree_listing(cache_path);
        CHECK_FALSE(before.empty());

        auto second = run_view(*set, View::from_file(gz));
        REQUIRE(second.has_value());
        CHECK(second->seed_source == SeedSource::Cache);
        CHECK(tree_listing(cache_path) == before);
    }

    TEST_CASE("a corrupt cache entry is a miss and the pass repairs it") {
        dftu_utils_test::TestEnvironment env(0);
        REQUIRE(env.is_valid());
        const dstd::String gz = write_trace(env, "corrupt", first_events());
        dftu_plugin plugin = make_seed_plugin();
        auto set = seed_set(&plugin);
        REQUIRE(set.has_value());
        g_contribute = false;

        g_seen.reset();
        auto first = run_view(*set, View::from_file(gz));
        REQUIRE(first.has_value());
        CHECK(first->seed_source == SeedSource::Pass);

        const auto plan = trace::views::detail::scan::from_file(
            gz, dstd::String(trace::internal::determine_index_path(gz, "")));
        const auto key = trace::index::cache::input_fingerprint(*plan);
        const dstd::String cache_path =
            trace::index::cache::phase_seed_cache_path(*plan);
        REQUIRE(key.has_value());
        auto db = trace::index::cache::CacheStore::open(
            cache_path, trace::index::cache::CacheStore::Mode::ReadWrite);
        REQUIRE(db);
        dstd::Vector<dstd::Pair<dstd::String, dstd::String>> entries;
        entries.emplace_back(dstd::String("\x01"), dstd::String("garbage"));
        REQUIRE(db->put(*key, entries, 64).has_value());

        g_seen.reset();
        auto second = run_view(*set, View::from_file(gz));
        REQUIRE(second.has_value());
        CHECK(second->seed_source == SeedSource::Pass);
        check_seen(reference_of({first_events()}));

        g_seen.reset();
        auto third = run_view(*set, View::from_file(gz));
        REQUIRE(third.has_value());
        CHECK(third->seed_source == SeedSource::Cache);
        check_seen(reference_of({first_events()}));
    }
}

namespace {

enum class Fault {
    WrongMerge,
    WrongType,
    NotDeclared,
    SkippedGeneration,
    SeededTwice,
    NotASeed,
    OtherPlugin
};
Fault g_fault = Fault::WrongMerge;

dftu_status declare_faulty(dftu_ctx* ctx, const dftu_plugin_host* host) {
    plugins::Ports ports(host, ctx);
    plugins::MergeOp op = plugins::MergeOp::Min;
    plugins::PhaseType type = plugins::PhaseType::U64;
    if (g_fault == Fault::WrongMerge) op = plugins::MergeOp::Max;
    if (g_fault == Fault::WrongType) type = plugins::PhaseType::I64;
    auto gen = ports.phase_declare("start", op, type);
    if (!gen) return plugins::status_of(gen.error());
    if (g_fault == Fault::SkippedGeneration) {
        auto second = ports.phase_declare("start", op, type);
        if (!second) return plugins::status_of(second.error());
        auto seeded = ports.seed("start", *second, plugins::PhaseSeed::TsMin);
        return seeded ? DFTU_OK : plugins::status_of(seeded.error());
    }
    const char* name = g_fault == Fault::NotDeclared   ? "ghost"
                       : g_fault == Fault::OtherPlugin ? "plugin.other.start"
                                                       : "start";
    const auto kind = g_fault == Fault::NotASeed
                          ? static_cast<plugins::PhaseSeed>(99)
                          : plugins::PhaseSeed::TsMin;
    auto seeded = ports.seed(name, *gen, kind);
    if (!seeded) return plugins::status_of(seeded.error());
    if (g_fault == Fault::SeededTwice) {
        auto again = ports.seed(name, *gen, kind);
        if (!again) return plugins::status_of(again.error());
    }
    return DFTU_OK;
}

dstd::String plan_error(Fault fault) {
    g_fault = fault;
    dftu_plugin plugin = make_seed_plugin();
    auto built = plugins::host::build_injected_plugins_declaring(
        {InjectedPlugin{&plugin, "seeder", declare_faulty}});
    if (built.has_value()) return {};
    return dstd::String(built.error().message());
}

}  // namespace

TEST_SUITE("phase seeds: plan rules") {
    TEST_CASE("a seed with another merge is a plan error naming the phase") {
        const dstd::String message = plan_error(Fault::WrongMerge);
        CHECK(has_text(message, "plugin.seeder.start"));
        CHECK(has_text(message, "TsMin"));
        CHECK(has_text(message, "merge"));
    }

    TEST_CASE("a seed with another value type is a plan error") {
        const dstd::String message = plan_error(Fault::WrongType);
        CHECK(has_text(message, "plugin.seeder.start"));
        CHECK(has_text(message, "U64"));
    }

    TEST_CASE("seeding a generation no one declared is a plan error") {
        const dstd::String message = plan_error(Fault::NotDeclared);
        CHECK(has_text(message, "plugin.seeder.ghost"));
        CHECK(has_text(message, "was not declared"));
    }

    TEST_CASE("a seed after an unseeded generation is a plan error") {
        const dstd::String message = plan_error(Fault::SkippedGeneration);
        CHECK(has_text(message, "plugin.seeder.start"));
        CHECK(has_text(message, "earlier generation"));
    }

    TEST_CASE("seeding a generation twice is a plan error") {
        const dstd::String message = plan_error(Fault::SeededTwice);
        CHECK(has_text(message, "plugin.seeder.start"));
        CHECK(has_text(message, "already seeded"));
    }

    TEST_CASE("an unknown seed kind is a plan error") {
        const dstd::String message = plan_error(Fault::NotASeed);
        CHECK(has_text(message, "plugin.seeder.start"));
        CHECK(has_text(message, "not a phase seed"));
    }

    TEST_CASE("only the owner seeds a generation") {
        const dstd::String message = plan_error(Fault::OtherPlugin);
        CHECK(has_text(message, "plugin.other.start"));
        CHECK(has_text(message, "only its owner"));
    }
}
