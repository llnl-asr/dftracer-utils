// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/domains/server/http_request.h>
#include <dftracer/utils/domains/server/http_response.h>
#include <dftracer/utils/domains/server/router.h>
#include <dftracer/utils/domains/server/trace_index.h>
#include <dftracer/utils/domains/server/viz/density.h>
#include <dftracer/utils/domains/server/viz_api.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/compress/gzip_rechunker.h>
#include <dftracer/utils/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <simdjson.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <algorithm>
#include <fstream>
#include <memory>

using namespace dftracer::utils;
using namespace dftracer::utils::domains::server;

namespace {

constexpr long INTERVAL = 5000;  // native units == us (US trace)

// A SELECTIVE-aggregation record (ph=3): individual events collapsed into
// count/duration stats plus extra per-arg reductions (tag_min, comm_min) that
// must survive to the client verbatim.
dstd::String agg_event(long ts, int dftu_cnt) {
    return "{\"name\":\"MPI_Irecv\",\"cat\":\"p2p\",\"type\":10,\"ts\":" +
           dstd::to_string(ts) + ",\"ph\":3,\"pid\":1,\"tid\":1,\"args\":{" +
           "\"dftu_cnt\":" + dstd::to_string(dftu_cnt) +
           ",\"dur_sum\":38,\"dur_min\":0,\"dur_max\":3,\"tag_min\":-1,"
           "\"comm_min\":-2080374783}}\n";
}

// end record declaring the aggregation window (trace_interval_ms is in ms).
dstd::String end_event(long ts) {
    return "{\"name\":\"end\",\"cat\":\"dftracer\",\"type\":1,\"ts\":" +
           dstd::to_string(ts) +
           ",\"dur\":0,\"ph\":1,\"pid\":1,\"tid\":1,\"args\":{\"cfg\":{"
           "\"trace_interval_ms\":5}}}\n";
}

dstd::String regular_event(long ts) {
    return "{\"name\":\"read\",\"cat\":\"POSIX\",\"type\":9,\"ts\":" +
           dstd::to_string(ts) + ",\"dur\":40,\"ph\":1,\"pid\":1,\"tid\":1}\n";
}

void write_single_member(const dstd::String& path,
                         const dstd::String& content) {
    fileio::compress::GzipMemberCompressor comp;
    auto member = comp.compress_member(content.data(), content.size());
    REQUIRE(member.has_value());
    dftu_utils_test::FileOut ofs(path);
    ofs.write(reinterpret_cast<const char*>(member->data()),
              static_cast<std::streamsize>(member->size()));
}

dstd::String density_body(Router& router, const dstd::String& target) {
    HttpRequest req;
    req.method = "GET";
    req.path = target;
    dstd::String body;
    Runtime rt(4);
    auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
        HttpResponse resp = co_await router.handle(req);
        CHECK(resp.status_code == 200);
        body = resp.body;
        co_return;
    });
    dftu_utils_test::ok(rt.submit(std::move(task), "density").wait());
    rt.shutdown();
    return body;
}

dstd::UniquePtr<TraceIndex> build_index(const dstd::String& content,
                                        const dstd::Path& dir) {
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    write_single_member(dstd::String((dir / "trace.pfw.gz").str()), content);
    auto index = dstd::make_unique<TraceIndex>(
        dstd::String(dir.str()), dstd::String((dir / "idx").str()));
    Runtime rt(4);
    auto task = coro::with_scope(
        [&](coro::Scope&) -> coro::CoroTask<dstd::Result<void>> {
            co_return co_await index->initialize();
        });
    dftu_utils_test::ok(rt.submit(std::move(task), "setup").wait());
    rt.shutdown();
    return index;
}

// Collect the est=true (extrapolated aggregate) events from a response body.
dstd::Vector<simdjson::dom::element> est_events(simdjson::dom::element root) {
    dstd::Vector<simdjson::dom::element> out;
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
        dstd::String trace = "[\n";
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

        dstd::String body = density_body(
            router,
            "/api/viz/density?begin=0&end=1000000&width=100&ts_normalize=0");

        simdjson::dom::parser parser;
        auto root = parser.parse(
            simdjson::padded_string(simdjson::padded_string(body)));
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
        dstd::String trace = "[\n";
        for (int k = 0; k < 3; ++k) trace += agg_event(100000, 10 + k);
        trace += end_event(105000);
        trace += "]\n";

        auto dir = dftu_utils_test::make_unique_test_path("viz_agg_cfg");
        auto index = build_index(trace, dir);
        Router router;
        register_viz_api(router, *index);

        dstd::String body = density_body(
            router,
            "/api/viz/density?begin=0&end=1000000&width=100&ts_normalize=0");

        simdjson::dom::parser parser;
        auto root = parser.parse(
            simdjson::padded_string(simdjson::padded_string(body)));
        auto est = est_events(root.value());
        REQUIRE(est.size() == 10 + 11 + 12);
        for (auto e : est) {
            double ts = e["ts"].get_double().value_unsafe();
            CHECK(ts >= 100000);
            CHECK(ts <= 105000);
        }
    }

    // A line with a huge number, a bad escape or a bad byte in a part the
    // density reads no value from is still an event of the window.
    TEST_CASE("density keeps events whose line is refused by a DOM parse") {
        dstd::String trace = "[\n";
        auto event = [](const char* name, long ts, long dur,
                        const char* extra) {
            return dstd::String("{\"name\":\"") + name +
                   "\",\"cat\":\"POSIX\",\"ph\":1,\"pid\":1,\"tid\":1,"
                   "\"ts\":" +
                   dstd::to_string(ts) + ",\"dur\":" + dstd::to_string(dur) +
                   extra + "}\n";
        };
        trace += event("a", 1000, 500, R"(,"args":{"x":1e400})");
        trace += event("b", 2000, 500, R"(,"args":{"s":"p\qq"})");
        trace += event("c", 3000, 500, R"(,"args":{"s":"p\ud800q"})");
        trace += event("d", 4000, 500,
                       R"(,"args":{"n":123456789012345678901234567890})");
        trace += event("e", 5000, 500, "");
        trace += event("sa", 6500, 1, R"(,"args":{"x":1e400})");
        trace += event("sb", 6600, 1, R"(,"args":{"s":"p\qq"})");
        trace += event("sc", 6700, 1,
                       ",\"args\":{\"s\":\"p\xff"
                       "q\"}");
        trace += R"({"name":"mem","cat":"C","ph":2,"pid":1,"tid":1,"ts":7000,)"
                 R"("args":{"free":5,"s":"x\q"}})"
                 "\n";
        trace += agg_event(7500, 5);
        trace += end_event(9000);
        trace += "]\n";

        auto dir = dftu_utils_test::make_unique_test_path("viz_refused");
        auto index = build_index(trace, dir);
        Router router;
        register_viz_api(router, *index);

        dstd::String body = density_body(
            router,
            "/api/viz/density?begin=0&end=10000&width=100&ts_normalize=0");

        dftracer::utils::json::Document document;
        auto parsed = document.parse(body);
        REQUIRE(parsed.has_value());
        dstd::Vector<dstd::String> names;
        auto events = parsed->find("events");
        REQUIRE(events.has_value());
        events->for_each_element([&](dftracer::utils::json::Value e) {
            if (e.find("est")) return;
            names.emplace_back(*e.find("name")->as_string());
        });
        std::sort(names.begin(), names.end());
        CHECK(names == dstd::Vector<dstd::String>{"a", "b", "c", "d", "e"});

        dstd::Vector<dstd::String> blocks;
        auto density = parsed->find("density");
        REQUIRE(density.has_value());
        density->for_each_element([&](dftracer::utils::json::Value blk) {
            const dstd::String name(*blk.find("name")->as_string());
            blocks.push_back(blk.find("counter") ? name + " (counter)" : name);
        });
        std::sort(blocks.begin(), blocks.end());
        CHECK(blocks == dstd::Vector<dstd::String>{"end", "mem.free (counter)",
                                                   "sa", "sb", "sc"});
    }

    TEST_CASE("density keeps an event nested far past the depth limit") {
        dstd::String deep;
        for (int i = 0; i < 1000000; ++i) deep += "[";
        deep += "1";
        for (int i = 0; i < 1000000; ++i) deep += "]";
        dstd::String trace = "[\n";
        trace += regular_event(1000);
        trace +=
            "{\"name\":\"nest\",\"cat\":\"POSIX\",\"ph\":1,\"pid\":1,"
            "\"tid\":1,\"ts\":2000,\"dur\":500,\"args\":{\"k\":" +
            deep + "}}\n";
        trace += agg_event(6000, 5);
        trace += end_event(9000);
        trace += "]\n";
        auto dir = dftu_utils_test::make_unique_test_path("viz_deep");
        auto index = build_index(trace, dir);
        Router router;
        register_viz_api(router, *index);
        dstd::String body = density_body(
            router,
            "/api/viz/density?begin=0&end=10000&width=100&ts_normalize=0");
        dftracer::utils::json::Document document;
        auto parsed = document.parse(body);
        REQUIRE(parsed.has_value());
        dstd::Vector<dstd::String> names;
        auto events = parsed->find("events");
        REQUIRE(events.has_value());
        events->for_each_element([&](dftracer::utils::json::Value e) {
            if (e.find("est")) return;
            names.emplace_back(*e.find("name")->as_string());
        });
        std::sort(names.begin(), names.end());
        CHECK(names == dstd::Vector<dstd::String>{"nest", "read"});
    }

    TEST_CASE("a repeated key in a density event keeps its first value") {
        dftracer::utils::json::Document document;
        auto parsed = document.parse(
            R"({"ph":1,"ts":1,"ts":2,"dur":5,"dur":6,"pid":4,"pid":5,"tid":7,"tid":8,"name":"a","name":"b"})");
        REQUIRE(parsed.has_value());
        DensityMap dens;
        double dur = 0;
        CHECK(fold_density(*parsed, 100, 0, dens, &dur) == DensityFold::Folded);
        CHECK(dur == 5);
        REQUIRE(dens.size() == 1);
        const auto& [key, agg] = *dens.begin();
        CHECK(key.pid == 4);
        CHECK(key.tid == 7);
        CHECK(key.col == 0);
        CHECK(agg.name == "a");

        DensityMap shifted;
        auto late = document.parse(R"({"ph":1,"ts":250,"ts":2,"dur":5})");
        REQUIRE(late.has_value());
        CHECK(fold_density(*late, 100, 0, shifted) == DensityFold::Folded);
        CHECK(shifted.begin()->first.col == 2);

        auto agg_line = document.parse(
            R"({"ts":3,"args":{"dftu_cnt":7,"dftu_cnt":9,"dur_sum":10,"dur_sum":20}})");
        REQUIRE(agg_line.has_value());
        dstd::Vector<AggRec> out;
        REQUIRE(collect_aggregated(*agg_line, "raw", out));
        REQUIRE(out.size() == 1);
        CHECK(out[0].count == 7);
        CHECK(out[0].total == 10);
    }
}
