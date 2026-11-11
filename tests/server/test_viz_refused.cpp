// SPDX-License-Identifier: MIT
// The viz folds read every line the scan hands them, also a line the DOM parser
// refuses: a number too large for a double, an invalid escape, a 20-digit
// integer, a bad UTF-8 or control byte, and a nesting past 1024.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/domains/server/http_request.h>
#include <dftracer/utils/domains/server/http_response.h>
#include <dftracer/utils/domains/server/router.h>
#include <dftracer/utils/domains/server/trace_index.h>
#include <dftracer/utils/domains/server/viz_api.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/record_schema.h>
#include <doctest/doctest.h>
#include <refused_lines.h>
#include <simdjson.h>
#include <task_result.h>
#include <testing_utilities.h>

using namespace dftracer::utils;
using namespace dftracer::utils::domains::server;

namespace {

void write_gz(const dstd::Path& path, const dstd::String& content) {
    fileio::compress::GzipMemberCompressor comp;
    auto member = comp.compress_member(content.data(), content.size());
    REQUIRE(member.has_value());
    dftu_utils_test::FileOut ofs(path);
    ofs.write(reinterpret_cast<const char*>(member->data()),
              static_cast<std::streamsize>(member->size()));
}

struct Served {
    explicit Served(const dstd::String& name) : dir(name) {}
    dftu_utils_test::ScopedTestDir dir;
    dstd::UniquePtr<TraceIndex> index;
    Router router;
};

dstd::UniquePtr<Served> serve(const dstd::String& name,
                              const dstd::String& content) {
    auto s = dstd::make_unique<Served>(name);
    write_gz(s->dir.path() / "trace.pfw.gz", content);
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

dstd::String get(Served& s, const dstd::String& target) {
    HttpRequest req;
    req.method = "GET";
    req.path = target;
    dstd::String body;
    Runtime rt(4);
    auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
        HttpResponse resp = co_await s.router.handle(req);
        CHECK_MESSAGE(resp.status_code == 200, target, ": ", resp.body);
        body = resp.body;
        co_return;
    });
    dftu_utils_test::ok(rt.submit(std::move(task), "get").wait());
    rt.shutdown();
    return body;
}

struct Parsed {
    simdjson::dom::parser parser;
    simdjson::dom::element root;
    explicit Parsed(const dstd::String& body) {
        REQUIRE(parser.parse(simdjson::padded_string(body)).get(root) ==
                simdjson::SUCCESS);
    }
};

// One complete event of the viz trace: name "read", category POSIX, tid 1.
void event(dstd::String& out, int pid, int ts, int dur,
           const dstd::String& args) {
    out += R"({"name":"read","cat":"POSIX","pid":)" + dstd::to_string(pid) +
           R"(,"tid":1,"ts":)" + dstd::to_string(ts) + R"(,"dur":)" +
           dstd::to_string(dur) + R"(,"ph":"X","args":)" + args + "}\n";
}

constexpr int ODD_FIRST_PID = 21;
constexpr int ODD_COUNT = 6;
constexpr int ODD_TS = 2000;
constexpr int ODD_DUR = 300;

dstd::String deep_args() {
    dstd::String args = R"({"ret":4096,"deep":)";
    for (int i = 0; i < 1100; ++i) args += '[';
    for (int i = 0; i < 1100; ++i) args += ']';
    return args + "}";
}

// Six lines of pid 1, then one line each of pids 21 to 26 that the DOM parser
// refuses, with the duration 300 plus its number.
dstd::String refused_trace() {
    dstd::String t = "[\n";
    for (int i = 0; i < 6; ++i)
        event(t, 1, 1000 + i * 10, 100, R"({"ret":4096})");
    const dstd::String args[ODD_COUNT] = {
        R"({"ret":4096,"n":1e400})",
        R"({"ret":4096,"s":"a\qb"})",
        R"({"ret":4096,"n":123456789012345678901234})",
        "{\"ret\":4096,\"s\":\"x\xffy\"}",
        "{\"ret\":4096,\"s\":\"a\tb\"}",
        deep_args()};
    for (int i = 0; i < ODD_COUNT; ++i)
        event(t, ODD_FIRST_PID + i, ODD_TS + i * 10, ODD_DUR + i, args[i]);
    return t + "]\n";
}

std::size_t count_of(const dstd::String& body, dstd::StringView needle) {
    std::size_t n = 0;
    for (auto at = body.find(needle); at != dstd::String::npos;
         at = body.find(needle, at + needle.size()))
        ++n;
    return n;
}

constexpr const char* WINDOW = "begin=0&end=100000&ts_normalize=0&summary=1";

struct Row {
    std::uint64_t count = 0;
    double total = 0;
};

dstd::Map<dstd::String, Row> stat_rows(const dstd::String& body) {
    Parsed p(body);
    dstd::Map<dstd::String, Row> out;
    for (auto r : p.root["names"].get_array())
        out[dstd::String(r["name"].get_string().value())] = {
            r["count"].get_uint64().value(), r["total"].get_double().value()};
    return out;
}

}  // namespace

TEST_SUITE("viz_refused") {
    TEST_CASE("the stats fold keeps every line of a window") {
        auto s = serve("viz_refused_stats", refused_trace());
        const dstd::String live =
            get(*s, dstd::String("/api/viz/stats?begin=1500&end=100000") +
                        "&ts_normalize=0&summary=1&group=pid");
        const auto rows = stat_rows(live);
        REQUIRE(rows.size() == ODD_COUNT);
        for (int i = 0; i < ODD_COUNT; ++i) {
            const auto it = rows.find(dstd::to_string(ODD_FIRST_PID + i));
            REQUIRE(it != rows.end());
            CHECK(it->second.count == 1);
            CHECK(it->second.total == ODD_DUR + i);
        }
    }

    TEST_CASE("the summary and the stats of a whole trace keep every line") {
        auto s = serve("viz_refused_summary", refused_trace());
        const auto rows = stat_rows(
            get(*s, dstd::String("/api/viz/stats?") + WINDOW + "&group=pid"));
        REQUIRE(rows.size() == 1 + ODD_COUNT);
        CHECK(rows.at("1").count == 6);
        CHECK(rows.at("1").total == 600);
        for (int i = 0; i < ODD_COUNT; ++i) {
            const auto it = rows.find(dstd::to_string(ODD_FIRST_PID + i));
            REQUIRE(it != rows.end());
            CHECK(it->second.count == 1);
            CHECK(it->second.total == ODD_DUR + i);
        }
    }

    TEST_CASE("the counters and the process tree keep every line") {
        auto s = serve("viz_refused_counters", refused_trace());
        Parsed counters(get(*s, dstd::String("/api/viz/counters?") + WINDOW));
        double ops = 0, read_bytes = 0;
        for (auto v : counters.root["ops"].get_array())
            ops += v.get_double().value();
        for (auto v : counters.root["read_bytes"].get_array())
            read_bytes += v.get_double().value();
        CHECK(ops == 6 + ODD_COUNT);
        CHECK(read_bytes == 4096.0 * (6 + ODD_COUNT));

        Parsed tree(get(*s, "/api/viz/proctree?ts_normalize=0"));
        dstd::Map<std::int64_t, Row> nodes;
        dstd::Map<std::int64_t, double> bytes;
        for (auto n : tree.root["nodes"].get_array()) {
            const std::int64_t pid = n["pid"].get_int64().value();
            nodes[pid] = {n["io_ops"].get_uint64().value(),
                          n["io_busy"].get_double().value()};
            bytes[pid] = n["bytes"].get_double().value();
        }
        REQUIRE(nodes.size() == 1 + ODD_COUNT);
        CHECK(nodes.at(1).count == 6);
        for (int i = 0; i < ODD_COUNT; ++i) {
            CHECK(nodes.at(ODD_FIRST_PID + i).count == 1);
            CHECK(nodes.at(ODD_FIRST_PID + i).total == ODD_DUR + i);
            CHECK(bytes.at(ODD_FIRST_PID + i) == 4096);
        }
    }

    // The window is narrow, so every event stays whole, and the odd line is
    // the only one that names the hash h599: a declaration is kept when an
    // event of the response refers to it, and the 500 first unreferenced
    // declarations are carried.
    TEST_CASE("density resolves and keeps the hashes of a refused line") {
        dstd::String t = "[\n";
        for (int i = 0; i < 600; ++i)
            for (const char* ph : {"M", "X"})
                t += R"({"name":"FH","cat":"dftracer","pid":1,"tid":1,)"
                     R"("ts":1500,"dur":5,"ph":")" +
                     dstd::String(ph) + R"(","args":{"name":"/f/)" +
                     dstd::to_string(i) + R"(","value":"h)" +
                     dstd::to_string(i) + "\"}}\n";
        for (int i = 0; i < 6; ++i)
            event(t, 1, 1000 + i * 10, 100, R"({"ret":4096,"fhash":"h1"})");
        event(t, 21, 2000, 300, R"({"ret":4096,"fhash":"h599","n":1e400})");
        t += "]\n";
        auto s = serve("viz_refused_density", t);
        const dstd::String body =
            get(*s,
                "/api/viz/density?begin=900&end=2100&width=1000"
                "&ts_normalize=0&summary=1&group_by=fhash");
        CHECK(count_of(body, R"("name":"FH")") == 502);
        CHECK(count_of(body, R"("value":"h599")") == 1);
        CHECK(count_of(body, R"("h1":"/f/1")") == 1);
        CHECK(count_of(body, R"("h599":"/f/599")") == 1);
    }

    TEST_CASE("a record schema serves a record the DOM parser refuses") {
        dftu_utils_test::ok_ref(
            trace::index::register_schema(R"(id: viz_refused_log
fields:
  t: {type: float, role: time, unit: s}
  took: {type: float, role: duration, unit: s}
  client: {type: string, role: entity}
  req: {type: string}
)",
                                          "test_viz_refused"));
        dstd::String t;
        constexpr double T0 = 1700000000.5;
        const char* const EXTRA[] = {"1e400", "\"a\\qb\"",
                                     "123456789012345678901234"};
        for (int i = 0; i < 30; ++i) {
            t += R"({"t":)" + dstd::to_string(T0 + i * 0.01) +
                 R"(,"took":0.004,"client":"10.0.0.)" + dstd::to_string(i % 3) +
                 R"(","req":"GET /a")";
            if (i >= 27) t += R"(,"extra":)" + dstd::String(EXTRA[i - 27]);
            t += "}\n";
        }
        auto s = serve("viz_refused_schema", t);
        CHECK(s->index->record_schema().id == "viz_refused_log");
        const dstd::String body =
            get(*s,
                "/api/viz/events?begin=0&end=10000000&summary=1"
                "&width=100000");
        CHECK(count_of(body, R"("dur":4000)") == 30);
    }

    // An aggregate (ph=3) whose duration sum is too large for a double reads
    // as infinity: its synthetic events keep every arg and carry no duration,
    // which the client reads from the sum, and the response stays valid JSON.
    TEST_CASE(
        "an aggregate with a number too large for a double extrapolates") {
        dstd::String trace = "[\n";
        const auto aggregate = [&](long ts, const char* count,
                                   const char* dur_sum) {
            trace += R"({"name":"MPI_Irecv","cat":"p2p","type":10,"ts":)" +
                     dstd::to_string(ts) +
                     R"(,"ph":3,"pid":1,"tid":1,"args":{"dftu_cnt":)" +
                     dstd::String(count) + R"(,"dur_sum":)" +
                     dstd::String(dur_sum) +
                     R"(,"tag_min":3}})"
                     "\n";
        };
        trace +=
            R"({"name":"read","cat":"POSIX","type":9,"ts":100000,"dur":40,)"
            R"("ph":1,"pid":1,"tid":1})"
            "\n";
        aggregate(100000, "20", "38");
        aggregate(105000, "20", "1e400");
        aggregate(110000, "20", "38");
        aggregate(115000, "1e400", "38");
        trace +=
            R"({"name":"end","cat":"dftracer","type":1,"ts":120000,"dur":0,)"
            R"("ph":1,"pid":1,"tid":1,"args":{"cfg":{"trace_interval_ms":5}}})"
            "\n]\n";
        auto s = serve("viz_refused_agg", trace);
        const dstd::String body =
            get(*s,
                "/api/viz/density?begin=0&end=1000000&width=100"
                "&ts_normalize=0");
        CHECK(count_of(body, R"("est":true)") == 61);
        CHECK(count_of(body, "inf") == 0);
        CHECK(count_of(body, R"("dur_sum":1e400,"tag_min":3},"est":true)") ==
              20);
        CHECK(count_of(body, R"("dur":1.900000,"est":true)") == 40);
        CHECK(count_of(body, R"("dur":38.000000,"est":true)") == 1);
    }

    TEST_CASE("the provenance graph keeps a record the DOM parser refuses") {
        dstd::String t = "[\n";
        for (int k = 0; k < dftu_utils_test::REFUSED_KINDS; ++k) {
            const dstd::String n = dstd::to_string(k);
            t += R"({"name":"EH","cat":"PROV","pid":1,"tid":1,"ts":)" +
                 dstd::to_string(1000 + k) + R"(,"ph":"M","args":{"id":"e)" +
                 n + R"(","type":"structure","store":"file","uri":"/u)" + n +
                 R"(",)" + dftu_utils_test::refused_member(k) + "}}\n";
        }
        t += "]\n";
        auto s = serve("viz_refused_prov", t);
        Parsed p(get(*s, "/api/prov/graph"));
        CHECK(p.root["stats"]["entities"].get_uint64().value() ==
              dftu_utils_test::REFUSED_KINDS);
        dstd::Set<dstd::String> uris;
        for (auto e : p.root["entities"].get_array())
            uris.insert(dstd::String(e["uri"].get_string().value()));
        CHECK(uris.size() == dftu_utils_test::REFUSED_KINDS);
        for (int k = 0; k < dftu_utils_test::REFUSED_KINDS; ++k)
            CHECK(uris.count("/u" + dstd::to_string(k)) == 1);
    }

    TEST_CASE("the stats keep a record nested 100000 deep") {
        dstd::String t = "[\n";
        event(t, 1, 1000, 100, R"({"ret":4096})");
        dstd::String args = R"({"ret":4096,"deep":)";
        args.append(100000, '[');
        args.append(100000, ']');
        args += "}";
        event(t, 41, 2000, 300, args);
        t += "]\n";
        auto s = serve("viz_refused_deep", t);
        const auto rows = stat_rows(
            get(*s, dstd::String("/api/viz/stats?begin=1500&end=100000") +
                        "&ts_normalize=0&summary=1&group=pid"));
        REQUIRE(rows.size() == 1);
        CHECK(rows.at("41").count == 1);
        CHECK(rows.at("41").total == 300);
    }
}
