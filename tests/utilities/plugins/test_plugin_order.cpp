// Fold ordering from the plugins' declared provides/consumes: a consumer
// registered before its producer still reads the producer's whole-scan
// accumulator, and the three graph faults (missing provider, duplicate
// provider, cycle) fail at build instead of after a wasted scan.

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
using dftracer::utils::plugins::fold_order;
using View = dftracer::utils::trace::views::View;
using ViewFile = dftracer::utils::trace::views::ViewFile;
using test_plugin_common::index_trace;
namespace coro = dftracer::utils::coro;

namespace {

constexpr const char* STATS_NAME = "com.example.order_stats";

void* make_empty_slice(void*) { return new int(0); }
void destroy_empty_slice(void* slice) { delete static_cast<int*>(slice); }
void no_merge(void*, void*) {}
void no_destroy(void*) {}
dftu_task* no_finalize(void*, const dftu_plugin_host*) { return nullptr; }

dftu_plugin make_bare_plugin(void* self) {
    dftu_plugin p{};
    p.abi_version = DFTRACER_PLUGIN_ABI_VERSION;
    p.self = self;
    p.make_slice = make_empty_slice;
    p.merge = no_merge;
    p.on_finalize = no_finalize;
    p.destroy_slice = destroy_empty_slice;
    p.destroy = no_destroy;
    return p;
}

// A plugin whose provides/consumes come from `self`, for the graph checks.
struct NameLists {
    std::vector<const char*> provides{nullptr};
    std::vector<const char*> consumes{nullptr};
};

const char* const* names_provides(void* self) {
    return static_cast<NameLists*>(self)->provides.data();
}
const char* const* names_consumes(void* self) {
    return static_cast<NameLists*>(self)->consumes.data();
}

dftu_plugin make_graph_plugin(NameLists* st) {
    dftu_plugin p = make_bare_plugin(st);
    p.provides = names_provides;
    p.consumes = names_consumes;
    return p;
}

// The producer: one zero-key accumulator over the whole scan, named
// STATS_NAME, which the consumer reads back by that name.
dftu_task* producer_on_batch(void*, const dftu_dataframe* df,
                             const dftu_plugin_host* host) {
    const auto* agg = static_cast<const dftu_svc_agg*>(
        host->get_service(host->h, DFTU_SVC_AGG));
    if (!agg || !agg->agg_new) return nullptr;
    const dftu_agg_col specs[1] = {
        {DFTU_AGG_COUNT, nullptr, "count", 0.0, nullptr}};
    if (dftu_agg* a = agg->agg_new(host->h, STATS_NAME, nullptr, 0, specs, 1))
        agg->agg_accumulate(host->h, a, df);
    return nullptr;
}

const char* const g_produced[2] = {STATS_NAME, nullptr};
const char* const* producer_provides(void*) { return g_produced; }

dftu_plugin make_producer() {
    dftu_plugin p = make_bare_plugin(nullptr);
    p.on_batch = producer_on_batch;
    p.provides = producer_provides;
    return p;
}

// The consumer: at finalize it fetches the producer's merged accumulator and
// records the row count it found. Zero means it ran first and saw nothing.
std::atomic<std::int64_t> g_seen_rows{-1};

dftu_task* consumer_on_finalize(void*, const dftu_plugin_host* host) {
    const auto* agg = static_cast<const dftu_svc_agg*>(
        host->get_service(host->h, DFTU_SVC_AGG));
    dftu_dataframe* res =
        agg && agg->agg_result ? agg->agg_result(host->h, STATS_NAME) : nullptr;
    g_seen_rows.store(
        res ? static_cast<std::int64_t>(dftu_dataframe_num_rows(res)) : 0);
    if (res) dftu_dataframe_free(res);
    return nullptr;
}

dftu_task* consumer_on_batch(void*, const dftu_dataframe*,
                             const dftu_plugin_host*) {
    return nullptr;
}

const char* const g_consumed[2] = {STATS_NAME, nullptr};
const char* const* consumer_consumes(void*) { return g_consumed; }

dftu_plugin make_consumer() {
    dftu_plugin p = make_bare_plugin(nullptr);
    p.on_batch = consumer_on_batch;
    p.on_finalize = consumer_on_finalize;
    p.consumes = consumer_consumes;
    return p;
}

std::string make_trace(dftu_utils_test::TestEnvironment& env,
                       const std::string& tag, int n) {
    std::string dir = env.get_dir() + "/" + tag;
    fs::create_directories(dir);
    std::string pfw = dir + "/trace.pfw";
    std::ofstream ofs(pfw);
    for (int i = 0; i < n; ++i)
        ofs << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
            << (1000 + i * 100) << R"(,"dur":)" << (10 + i) << R"(,"args":{}})"
            << "\n";
    ofs.close();
    std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip(pfw, gz);
    fs::remove(pfw);
    return gz;
}

void run_host(std::vector<dftu_plugin*> plugins, std::vector<ViewFile> files) {
    auto set = build_injected_plugins(std::move(plugins));
    REQUIRE(set.has_value());
    View view = View::from_files(std::move(files));
    Runtime rt(4);
    auto task = dftracer::utils::run_coro_scope(
        rt.executor(), [&](CoroScope&) -> coro::CoroTask<void> {
            auto run = co_await set->run(view);
            REQUIRE(run.has_value());
            co_return;
        });
    rt.submit(std::move(task), "plugin-order").wait();
    rt.shutdown();
}

}  // namespace

TEST_SUITE("PluginFoldOrder") {
    TEST_CASE("a consumer registered first still reads its producer") {
        dftu_utils_test::TestEnvironment env(40);
        ViewFile file = index_trace(make_trace(env, "t", 40));

        dftu_plugin consumer = make_consumer();
        dftu_plugin producer = make_producer();

        // Deliberately the wrong order: the consumer is given first, as a
        // "--plugin consumer --plugin producer" command line would.
        g_seen_rows.store(-1);
        run_host({&consumer, &producer}, {file});
        CHECK(g_seen_rows.load() == 1);

        // The natural order must agree.
        g_seen_rows.store(-1);
        run_host({&producer, &consumer}, {file});
        CHECK(g_seen_rows.load() == 1);
    }

    TEST_CASE("the provider is ordered before its consumer") {
        dftu_plugin consumer = make_consumer();
        dftu_plugin producer = make_producer();
        auto set = build_injected_plugins({&consumer, &producer});
        REQUIRE(set.has_value());
        std::vector<std::size_t> order = fold_order(*set);
        REQUIRE(order.size() == 2);
        CHECK(order[0] == 1);
        CHECK(order[1] == 0);
    }

    TEST_CASE("independent plugins keep their registration order") {
        NameLists a, b;
        dftu_plugin pa = make_graph_plugin(&a);
        dftu_plugin pb = make_graph_plugin(&b);
        auto set = build_injected_plugins({&pa, &pb});
        REQUIRE(set.has_value());
        std::vector<std::size_t> order = fold_order(*set);
        REQUIRE(order.size() == 2);
        CHECK(order[0] == 0);
        CHECK(order[1] == 1);
    }

    TEST_CASE("a consumed name no plugin provides fails the build") {
        NameLists consumer;
        consumer.consumes = {"com.example.absent", nullptr};
        dftu_plugin p = make_graph_plugin(&consumer);
        auto set = build_injected_plugins({&p});
        REQUIRE_FALSE(set.has_value());
        CHECK(set.error().message.find("com.example.absent") !=
              std::string::npos);
        CHECK(set.error().message.find("no loaded plugin provides") !=
              std::string::npos);
    }

    TEST_CASE("two plugins providing the same name fail the build") {
        NameLists a, b;
        a.provides = {"com.example.dup", nullptr};
        b.provides = {"com.example.dup", nullptr};
        dftu_plugin pa = make_graph_plugin(&a);
        dftu_plugin pb = make_graph_plugin(&b);
        auto set = build_injected_plugins({&pa, &pb});
        REQUIRE_FALSE(set.has_value());
        CHECK(set.error().message.find("com.example.dup") != std::string::npos);
        CHECK(set.error().message.find("both provide") != std::string::npos);
    }

    TEST_CASE("a provides/consumes cycle fails the build") {
        NameLists a, b;
        a.provides = {"com.example.a", nullptr};
        a.consumes = {"com.example.b", nullptr};
        b.provides = {"com.example.b", nullptr};
        b.consumes = {"com.example.a", nullptr};
        dftu_plugin pa = make_graph_plugin(&a);
        dftu_plugin pb = make_graph_plugin(&b);
        auto set = build_injected_plugins({&pa, &pb});
        REQUIRE_FALSE(set.has_value());
        CHECK(set.error().message.find("cycle") != std::string::npos);
    }

    TEST_CASE("a dftu.-prefixed provided name fails the build") {
        NameLists a;
        a.provides = {"dftu.hash.fnv1a", nullptr};
        dftu_plugin pa = make_graph_plugin(&a);
        auto set = build_injected_plugins({&pa});
        REQUIRE_FALSE(set.has_value());
        CHECK(set.error().message.find("dftu.hash.fnv1a") != std::string::npos);
        CHECK(set.error().message.find("belongs to the host") !=
              std::string::npos);
    }

    TEST_CASE("a dftu.-prefixed consumed name fails the build") {
        NameLists a;
        a.consumes = {"dftu.svc.agg", nullptr};
        dftu_plugin pa = make_graph_plugin(&a);
        auto set = build_injected_plugins({&pa});
        REQUIRE_FALSE(set.has_value());
        CHECK(set.error().message.find("dftu.svc.agg") != std::string::npos);
        CHECK(set.error().message.find("belongs to the host") !=
              std::string::npos);
    }

    TEST_CASE("a plugin consuming its own name is not a cycle") {
        NameLists a;
        a.provides = {"com.example.self", nullptr};
        a.consumes = {"com.example.self", nullptr};
        dftu_plugin pa = make_graph_plugin(&a);
        auto set = build_injected_plugins({&pa});
        REQUIRE(set.has_value());
        CHECK(fold_order(*set).size() == 1);
    }
}
