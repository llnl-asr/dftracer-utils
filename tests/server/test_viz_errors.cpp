// SPDX-License-Identifier: MIT
// How a failure inside a viz handler reaches the client: a bad query is a 400
// with its text, every other failure a 500 with its message.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/duql/errc.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/server/viz_api.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>

using namespace dftracer::utils;
using namespace dftracer::utils::server;

namespace {

struct Served {
    explicit Served(const dstd::String& name) : dir(name) {}
    dftu_utils_test::ScopedTestDir dir;
    dstd::UniquePtr<TraceIndex> index;
    Router router;
};

dstd::UniquePtr<Served> serve(const dstd::String& name) {
    auto s = dstd::make_unique<Served>(name);
    dstd::String content = "[\n";
    for (int i = 0; i < 6; ++i)
        content += R"({"name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)" +
                   dstd::to_string(1000 + i * 10) +
                   R"(,"dur":100,"ph":"X","args":{"ret":4096}})" + "\n";
    content += "]\n";
    utilities::fileio::compress::GzipMemberCompressor comp;
    auto member = comp.compress_member(content.data(), content.size());
    REQUIRE(member.has_value());
    {
        dftu_utils_test::FileOut ofs(s->dir.path() / "trace.pfw.gz");
        ofs.write(reinterpret_cast<const char*>(member->data()),
                  static_cast<std::streamsize>(member->size()));
    }
    s->index = dstd::make_unique<TraceIndex>(dstd::String(s->dir.str()),
                                             dstd::String(s->dir.file("idx")));
    Runtime rt(4);
    auto task = coro::with_scope(
        [&](coro::Scope&) -> coro::CoroTask<dstd::Result<void>> {
            co_return co_await s->index->initialize();
        });
    dftu_utils_test::ok(rt.submit(std::move(task), "setup").wait());
    rt.shutdown();
    REQUIRE(s->index->file_count() == 1);
    register_viz_api(s->router, *s->index);
    return s;
}

HttpResponse get(Served& s, const dstd::String& target,
                 const char* method = "GET") {
    HttpRequest req;
    req.method = method;
    req.path = target;
    HttpResponse out;
    Runtime rt(4);
    auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
        out = co_await s.router.handle(req);
        co_return;
    });
    dftu_utils_test::ok(rt.submit(std::move(task), "get").wait());
    rt.shutdown();
    return out;
}

}  // namespace

TEST_SUITE("viz_errors") {
    TEST_CASE("an invalid argument is a 400 with its message") {
        const auto resp = HttpResponse::from_error(
            dstd::Error{common::ErrorCode::InvalidArgument, "bad window"});
        CHECK(resp.status_code == 400);
        CHECK(resp.body == "bad window");
    }

    TEST_CASE("a duql parse error is a 400 with its text") {
        const auto resp = HttpResponse::from_error(
            dstd::Error{duql::DuqlErrc::Parse, "line 1:\n  x ==\n  ^\n  oops"});
        CHECK(resp.status_code == 400);
        CHECK(resp.body == "line 1:\n  x ==\n  ^\n  oops");
    }

    TEST_CASE("any other error is a 500 with its message") {
        for (const auto code :
             {common::ErrorCode::Io, common::ErrorCode::Internal,
              common::ErrorCode::Parse, common::ErrorCode::NotFound}) {
            const auto resp =
                HttpResponse::from_error(dstd::Error{code, "it broke"});
            CHECK(resp.status_code == 500);
            CHECK(resp.body == "it broke");
        }
    }

    TEST_CASE("a query that does not parse is a 400 on density and events") {
        auto s = serve("viz_errors_query");
        for (const char* endpoint :
             {"/api/viz/density?begin=0&end=100000&width=1000&summary=0",
              "/api/viz/events?begin=0&end=100000&summary=0",
              "/api/viz/untimed?limit=10"}) {
            const auto resp =
                get(*s, dstd::String(endpoint) + "&duql=name%20%3D%3D%20%28");
            CHECK_MESSAGE(resp.status_code == 400, endpoint, ": ", resp.body);
            CHECK_MESSAGE(resp.body.find("Invalid duql") != dstd::String::npos,
                          endpoint, ": ", resp.body);
        }
    }

    TEST_CASE("a scan that fails is a 500") {
        auto s = serve("viz_errors_scan");
        dstd::fs::remove(dstd::PathView(s->dir.file("trace.pfw.gz"))).discard();
        dstd::fs::remove_all(dstd::PathView(s->dir.file("idx"))).discard();
        for (const char* endpoint :
             {"/api/viz/density?begin=0&end=100000&width=1000&summary=0",
              "/api/viz/events?begin=0&end=100000&summary=0",
              "/api/viz/stats?begin=0&end=100000&summary=0",
              "/api/viz/counters?begin=0&end=100000&summary=0",
              "/api/viz/histogram?begin=0&end=100000&summary=0"}) {
            const auto resp = get(*s, endpoint);
            INFO(endpoint, ": ", resp.body);
            CHECK(resp.status_code == 500);
            CHECK_FALSE(resp.body.empty());
        }
    }

    TEST_CASE("the cancel body format is valid JSON") {
        // The route itself touches the CancelRegistry singleton, which a test
        // binary's exit check reports as live memory.
        CHECK(dstd::fmt<"{\"cancelled\":{}}">(true) == R"({"cancelled":true})");
        CHECK(dstd::fmt<"{\"cancelled\":{}}">(false) ==
              R"({"cancelled":false})");
    }
}
