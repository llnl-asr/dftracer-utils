#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/server/viz_api.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_rechunker.h>
#include <doctest/doctest.h>
#include <simdjson.h>
#include <testing_utilities.h>

#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace dftracer::utils;
using dftracer::utils::server::HttpRequest;
using dftracer::utils::server::HttpResponse;
using dftracer::utils::server::register_viz_api;
using dftracer::utils::server::Router;
using dftracer::utils::server::TraceIndex;

namespace {

constexpr long INTERVAL = 5000;  // native units == us (US trace)

// A SELECTIVE-aggregation record (ph=3): individual events collapsed into
// count/duration stats plus extra per-arg reductions (tag_min, comm_min) that
// must survive to the client verbatim.
std::string agg_event(long ts, int dftu_cnt) {
    return "{\"name\":\"MPI_Irecv\",\"cat\":\"p2p\",\"type\":10,\"ts\":" +
           std::to_string(ts) + ",\"ph\":3,\"pid\":1,\"tid\":1,\"args\":{" +
           "\"dftu_cnt\":" + std::to_string(dftu_cnt) +
           ",\"dur_sum\":38,\"dur_min\":0,\"dur_max\":3,\"tag_min\":-1,"
           "\"comm_min\":-2080374783}}\n";
}

// end record declaring the aggregation window (trace_interval_ms is in ms).
std::string end_event(long ts) {
    return "{\"name\":\"end\",\"cat\":\"dftracer\",\"type\":1,\"ts\":" +
           std::to_string(ts) +
           ",\"dur\":0,\"ph\":1,\"pid\":1,\"tid\":1,\"args\":{\"cfg\":{"
           "\"trace_interval_ms\":5}}}\n";
}

std::string regular_event(long ts) {
    return "{\"name\":\"read\",\"cat\":\"POSIX\",\"type\":9,\"ts\":" +
           std::to_string(ts) + ",\"dur\":40,\"ph\":1,\"pid\":1,\"tid\":1}\n";
}

void write_single_member(const std::string& path, const std::string& content) {
    using dftracer::utils::utilities::fileio::compress::GzipMemberCompressor;
    GzipMemberCompressor comp;
    auto member = comp.compress_member(content.data(), content.size());
    REQUIRE(member.has_value());
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(member->data()),
              static_cast<std::streamsize>(member->size()));
}

std::string density_body(Router& router, const std::string& target) {
    HttpRequest req;
    req.method = "GET";
    req.path = target;
    std::string body;
    Runtime rt(4);
    auto task =
        run_coro_scope(rt.executor(), [&](CoroScope&) -> coro::CoroTask<void> {
            HttpResponse resp = co_await router.handle(req);
            CHECK(resp.status_code == 200);
            body = resp.body;
            co_return;
        });
    rt.submit(std::move(task), "density").wait();
    rt.shutdown();
    return body;
}

std::unique_ptr<TraceIndex> build_index(const std::string& content,
                                        const fs::path& dir) {
    fs::create_directories(dir);
    write_single_member((dir / "trace.pfw.gz").string(), content);
    auto index =
        std::make_unique<TraceIndex>(dir.string(), (dir / "idx").string());
    Runtime rt(4);
    auto task =
        run_coro_scope(rt.executor(), [&](CoroScope&) -> coro::CoroTask<void> {
            co_await index->initialize();
        });
    rt.submit(std::move(task), "setup").wait();
    rt.shutdown();
    return index;
}

// Collect the est=true (extrapolated aggregate) events from a response body.
std::vector<simdjson::dom::element> est_events(simdjson::dom::element root) {
    std::vector<simdjson::dom::element> out;
    for (auto e : root["events"].get_array()) {
        auto a = e["est"];
        if (!a.error() && a.get_bool().value_unsafe()) out.push_back(e);
    }
    return out;
}

}  // namespace

TEST_SUITE("viz_aggregated") {
    // ph=3 records surface on the unfiltered density path (the summary's
    // has_aggregated flag routes past the duration-built pyramid) and, since
    // the viewport is wider than the window here, extrapolate into one
    // positioned synthetic event per collapsed event, each keeping every arg.
    TEST_CASE("aggregates extrapolate into positioned events with full args") {
        std::string trace = "[\n";
        trace += regular_event(100000);
        for (int k = 0; k < 4; ++k)
            trace += agg_event(100000 + k * INTERVAL, 20 + k);
        trace += end_event(100000 + 4 * INTERVAL);
        trace += "]\n";

        auto dir = dftu_utils_test::make_unique_test_path("viz_agg");
        auto index = build_index(trace, dir);
        REQUIRE(index->file_count() == 1);

        Router router;
        register_viz_api(router, *index);

        std::string body = density_body(
            router,
            "/api/viz/density?begin=0&end=1000000&width=100&ts_normalize=0");

        simdjson::dom::parser parser;
        auto root = parser.parse(body);
        auto est = est_events(root.value());
        // dftu_cnt of 20, 21, 22, 23 -> one synthetic event each.
        REQUIRE(est.size() == 20 + 21 + 22 + 23);

        for (auto e : est) {
            double ts = e["ts"].get_double().value_unsafe();
            CHECK(ts >= 100000);
            CHECK(ts < static_cast<double>(100000 + 5 * INTERVAL));
            CHECK(e["dur"].get_double().value_unsafe() > 0);
            auto args = e["args"];
            REQUIRE(!args.error());
            CHECK(!args["dftu_cnt"].error());
            CHECK(!args["dur_sum"].error());
            CHECK(!args["tag_min"].error());
            CHECK(!args["comm_min"].error());
        }
    }

    // A single occupied window gives no gap to infer from, so the interval
    // falls back to the trace's declared cfg.trace_interval_ms; the events
    // still extrapolate across that window.
    TEST_CASE("interval falls back to cfg when only one window is present") {
        std::string trace = "[\n";
        for (int k = 0; k < 3; ++k) trace += agg_event(100000, 10 + k);
        trace += end_event(105000);
        trace += "]\n";

        auto dir = dftu_utils_test::make_unique_test_path("viz_agg_cfg");
        auto index = build_index(trace, dir);
        Router router;
        register_viz_api(router, *index);

        std::string body = density_body(
            router,
            "/api/viz/density?begin=0&end=1000000&width=100&ts_normalize=0");

        simdjson::dom::parser parser;
        auto root = parser.parse(body);
        auto est = est_events(root.value());
        REQUIRE(est.size() == 10 + 11 + 12);
        for (auto e : est) {
            double ts = e["ts"].get_double().value_unsafe();
            CHECK(ts >= 100000);
            CHECK(ts <= 105000);
        }
    }
}
