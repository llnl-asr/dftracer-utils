#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/runtime/coro/coro.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/server/viz_api.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_rechunker.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <fstream>
#include <string>
#include <vector>

using namespace dftracer::utils;
using dftracer::utils::server::HttpRequest;
using dftracer::utils::server::HttpResponse;
using dftracer::utils::server::register_viz_api;
using dftracer::utils::server::Router;
using dftracer::utils::server::TraceIndex;

namespace {

// One .pfw line. Duration events (ph=X) carry per-name-distinct durations so no
// two names share a total (keeps the total-sorted output order deterministic).
// A counter (ph=C, dur:0) is sprinkled in to exercise the has_dur path: the viz
// stats fold counts a dur:0 event, and so must the stored per-chunk aggregate.
std::string make_trace(int n) {
    const char* names[] = {"read", "write", "open"};
    const char* cats[] = {"POSIX", "STDIO", "MPI"};
    const int durs[] = {100, 250, 40};
    std::string s = "[\n";
    for (int i = 0; i < n; ++i) {
        int k = i % 3;
        // name/cat/pid vary together so each group's per-key totals are
        // distinct (no ties -> deterministic total-sorted output order).
        s += "{\"id\":" + std::to_string(i) + ",\"name\":\"" + names[k] +
             "\",\"cat\":\"" + cats[k] + "\",\"pid\":" + std::to_string(1 + k) +
             ",\"tid\":1,\"ts\":" + std::to_string(1000 + i) +
             ",\"dur\":" + std::to_string(durs[k]) + ",\"ph\":\"X\"}\n";
        if (i % 50 == 0) {
            s += "{\"id\":" + std::to_string(i) +
                 ",\"name\":\"cpu\",\"cat\":\"sys\",\"pid\":9,\"tid\":1,"
                 "\"ts\":" +
                 std::to_string(1000 + i) + ",\"dur\":0,\"ph\":\"C\"}\n";
        }
    }
    return s;
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

std::string stats_body(Router& router, const std::string& target) {
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
    rt.submit(std::move(task), "stats").wait();
    rt.shutdown();
    return body;
}

}  // namespace

TEST_SUITE("viz_stats_fastpath") {
    // The stored-aggregate fast path must produce byte-identical output to a
    // full live decode (nofast=1) for every unfiltered group-by-name window.
    TEST_CASE("fast path equals live scan over a multi-member trace") {
        const int n = 3000;
        auto dir = dftu_utils_test::make_unique_test_path("viz_stats_fast");
        fs::create_directories(dir);
        // Not *.pfw.gz: TraceIndex scans the dir and must see only the trace.
        const std::string src = (dir / "src.raw.gz").string();
        const std::string gz = (dir / "trace.pfw.gz").string();
        const std::string index_dir = (dir / "idx").string();

        write_single_member(src, make_trace(n));

        TraceIndex index(dir.string(), index_dir);
        {
            // Rechunk into many small members, then build the index; both need
            // a runtime. Members are the atomic decode/aggregate unit, so
            // several must sit fully inside a window for the fast path to skip
            // decodes.
            namespace gzc = utilities::fileio::compress;
            Runtime rt(4);
            auto task = run_coro_scope(
                rt.executor(), [&](CoroScope&) -> coro::CoroTask<void> {
                    co_await gzc::gzip_rechunk_to_members(
                        src, gz, /*member_size=*/4096, /*level=*/6);
                    co_await index.initialize();
                    co_return;
                });
            rt.submit(std::move(task), "setup").wait();
            rt.shutdown();
        }
        REQUIRE(index.file_count() == 1);

        Router router;
        register_viz_api(router, index);

        // ts_normalize=0 keeps begin/end in the trace's own units (ts=1000+i).
        struct Window {
            long begin;
            long end;
        };
        const std::vector<Window> windows = {
            {0, 1000000},                // whole trace
            {1500, 3500},                // many fully-covered interior members
            {2000, 4000}, {2345, 4111},  // off-boundary edges
            {2000, 2100},                // narrow: falls inside a single member
            {3900, 4100},                // near the tail
        };

        // Every fast-path group (name/cat/pid) must match its live scan.
        const char* groups[] = {"name", "cat", "pid"};
        for (const auto& w : windows) {
            for (const char* g : groups) {
                std::string q =
                    "/api/viz/stats?begin=" + std::to_string(w.begin) +
                    "&end=" + std::to_string(w.end) +
                    "&summary=1&ts_normalize=0&group=" + g;
                std::string fast = stats_body(router, q);
                std::string live = stats_body(router, q + "&nofast=1");
                INFO("group=" << g << " window [" << w.begin << ", " << w.end
                              << "]");
                CHECK(fast == live);
            }
        }
    }
}
