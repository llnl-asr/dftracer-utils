// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/server/viz_api.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_rechunker.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <fstream>

using namespace dftracer::utils;
using namespace dftracer::utils::server;

namespace {

// One .pfw line. Duration events (ph=X) carry per-name-distinct durations so no
// two names share a total (keeps the total-sorted output order deterministic).
// A counter (ph=C, dur:0) is sprinkled in to exercise the has_dur path: the viz
// stats fold counts a dur:0 event, and so must the stored per-chunk aggregate.
dstd::String make_trace(int n) {
    const char* names[] = {"read", "write", "open"};
    const char* cats[] = {"POSIX", "STDIO", "MPI"};
    const int durs[] = {100, 250, 40};
    dstd::String s = "[\n";
    for (int i = 0; i < n; ++i) {
        int k = i % 3;
        // name/cat/pid vary together so each group's per-key totals are
        // distinct (no ties -> deterministic total-sorted output order).
        s += "{\"id\":" + dstd::to_string(i) + ",\"name\":\"" + names[k] +
             "\",\"cat\":\"" + cats[k] +
             "\",\"pid\":" + dstd::to_string(1 + k) +
             ",\"tid\":1,\"ts\":" + dstd::to_string(1000 + i) +
             ",\"dur\":" + dstd::to_string(durs[k]) + ",\"ph\":\"X\"}\n";
        if (i % 50 == 0) {
            s += "{\"id\":" + dstd::to_string(i) +
                 ",\"name\":\"cpu\",\"cat\":\"sys\",\"pid\":9,\"tid\":1,"
                 "\"ts\":" +
                 dstd::to_string(1000 + i) + ",\"dur\":0,\"ph\":\"C\"}\n";
        }
    }
    return s;
}

void write_single_member(const dstd::String& path,
                         const dstd::String& content) {
    utilities::fileio::compress::GzipMemberCompressor comp;
    auto member = comp.compress_member(content.data(), content.size());
    REQUIRE(member.has_value());
    dftu_utils_test::FileOut ofs(path);
    ofs.write(reinterpret_cast<const char*>(member->data()),
              static_cast<std::streamsize>(member->size()));
}

dstd::String stats_body(Router& router, const dstd::String& target) {
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
    dftu_utils_test::ok(rt.submit(std::move(task), "stats").wait());
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
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        // Not a .gz file: TraceIndex scans the dir and must see only the trace.
        const dstd::String src = dstd::String((dir / "src.raw").str());
        const dstd::String gz = dstd::String((dir / "trace.pfw.gz").str());
        const dstd::String index_dir = dstd::String((dir / "idx").str());

        write_single_member(src, make_trace(n));

        TraceIndex index(dstd::String(dir.str()), dstd::String(index_dir));
        {
            // Rechunk into many small members, then build the index; both need
            // a runtime. Members are the atomic decode/aggregate unit, so
            // several must sit fully inside a window for the fast path to skip
            // decodes.
            Runtime rt(4);
            auto task = coro::with_scope(
                [&](coro::Scope&) -> coro::CoroTask<dstd::Result<void>> {
                    auto rechunked = co_await utilities::fileio::compress::
                        gzip_rechunk_to_members(dstd::String(src),
                                                dstd::String(gz),
                                                /*member_size=*/4096,
                                                /*level=*/6);
                    if (!rechunked) co_return rechunked.error();
                    co_return co_await index.initialize();
                });
            dftu_utils_test::ok(rt.submit(std::move(task), "setup").wait());
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
        const dstd::Vector<Window> windows = {
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
                dstd::String q =
                    "/api/viz/stats?begin=" + dstd::to_string(w.begin) +
                    "&end=" + dstd::to_string(w.end) +
                    "&summary=1&ts_normalize=0&group=" + g;
                dstd::String fast = stats_body(router, q);
                dstd::String live = stats_body(router, q + "&nofast=1");
                INFO("group=" << g << " window [" << w.begin << ", " << w.end
                              << "]");
                CHECK(fast == live);
            }
        }
    }
}
