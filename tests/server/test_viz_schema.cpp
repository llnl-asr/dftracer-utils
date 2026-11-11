// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/detail/coro.h>
#include <dftracer/utils/domains/server/http_request.h>
#include <dftracer/utils/domains/server/http_response.h>
#include <dftracer/utils/domains/server/router.h>
#include <dftracer/utils/domains/server/trace_api.h>
#include <dftracer/utils/domains/server/trace_index.h>
#include <dftracer/utils/domains/server/viz_api.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/compress/gzip_rechunker.h>
#include <dftracer/utils/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/record_schema.h>
#include <doctest/doctest.h>
#include <simdjson.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>

using namespace dftracer::utils;
using namespace dftracer::utils::domains::server;

namespace {

// A web access log: float seconds for time and duration, a text client as
// the entity, and a hosts row set naming each client's server. Records
// without a time are untimed.
constexpr const char* WEBLOG_SCHEMA = R"(id: srv_weblog
fields:
  t: {type: float, role: time, unit: s, optional: true}
  took: {type: float, role: duration, unit: s}
  client: {type: string, role: entity}
  req: {type: string}
  status: {type: int}
  host: {type: string}
source: "hosts = select client, name = host | distinct"
)";

// OpenTelemetry-like spans: nested fields and nanosecond times.
constexpr const char* OTEL_SCHEMA = R"(id: srv_otel
fields:
  name: {type: string}
  start: {type: int, path: span.start_ns, role: time, unit: ns}
  length: {type: int, path: span.dur_ns, role: duration, unit: ns}
  service: {type: string, path: resource.service, role: entity}
)";

// The parity twin: dftracer's pid, ts, dur and name under other names.
constexpr const char* TWIN_SCHEMA = R"(id: srv_twin
fields:
  op: {type: string}
  at: {type: int, role: time, unit: us}
  len: {type: int, role: duration, unit: us}
  who: {type: int, role: entity}
)";

void register_schemas() {
    for (const char* spec : {WEBLOG_SCHEMA, OTEL_SCHEMA, TWIN_SCHEMA})
        dftu_utils_test::ok_ref(
            trace::index::register_schema(spec, "test_viz_schema"));
}

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

// A server over `content`, written as the one trace file of a fresh
// directory. Discovery lists .pfw.gz files only.
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
    domains::server::register_trace_api(s->router, *s->index);
    domains::server::register_viz_api(s->router, *s->index);
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

dstd::String file_of(Served& s) {
    return dstd::String(s.index->files().front().path);
}

constexpr int WEB_RECORDS = 300;
constexpr int WEB_UNTIMED = 3;
constexpr double WEB_T0 = 1700000000.5;

dstd::String weblog() {
    dstd::String out;
    for (int i = 0; i < WEB_RECORDS; ++i) {
        out += R"({"t":)" + dstd::to_string(WEB_T0 + i * 0.01);
        out += R"(,"took":)" + dstd::to_string(0.002 + (i % 5) * 0.001);
        out += R"(,"client":"10.0.0.)" + dstd::to_string(i % 3) + '"';
        out += R"(,"req":")" + dstd::String(i % 2 ? "GET /a" : "POST /b") + '"';
        out +=
            R"(,"status":200,"host":"web-)" + dstd::to_string(i % 3) + "\"}\n";
    }
    for (int i = 0; i < WEB_UNTIMED; ++i)
        out += R"({"took":0.5,"client":"10.0.0.9","req":"GET /late",)"
               R"("status":504,"host":"web-9"})"
               "\n";
    return out;
}

constexpr int OTEL_REQUESTS = 50;
constexpr std::int64_t OTEL_T0 = 1700000000000000000;

dstd::String otel() {
    dstd::String out;
    auto span = [&](const char* name, const char* service, std::int64_t start,
                    std::int64_t dur) {
        out += R"({"name":")" + dstd::String(name) +
               R"(","span":{"start_ns":)" + dstd::to_string(start) +
               R"(,"dur_ns":)" + dstd::to_string(dur) +
               R"(},"resource":{"service":")" + service + "\"}}\n";
    };
    for (int i = 0; i < OTEL_REQUESTS; ++i) {
        const std::int64_t t = OTEL_T0 + i * 20'000'000LL;
        span("handle", "api", t, 10'000'000);
        span("db.query", "api", t + 1'000'000, 2'000'000);
        span("render", "api", t + 4'000'000, 5'000'000);
        span("exec", "db", t + 1'100'000, 1'500'000);
    }
    return out;
}

// One process per pid, each running open > (read, write) per step.
struct TwinEvent {
    const char* name;
    std::int64_t pid;
    std::int64_t ts;
    std::int64_t dur;
};

dstd::Vector<TwinEvent> twin_events() {
    dstd::Vector<TwinEvent> out;
    for (std::int64_t pid : {101, 102})
        for (std::int64_t i = 0; i < 200; ++i) {
            const std::int64_t t = 1'000'000 + i * 1000 + (pid - 101) * 7;
            out.push_back({"open", pid, t, 800});
            out.push_back({"read", pid, t + 100, 300});
            out.push_back({"write", pid, t + 450, 200});
        }
    return out;
}

dstd::String twin_dftracer() {
    dstd::String out = "[\n";
    for (const auto& e : twin_events())
        out += R"({"name":")" + dstd::String(e.name) +
               R"(","cat":"POSIX","ph":1,"pid":)" + dstd::to_string(e.pid) +
               R"(,"tid":)" + dstd::to_string(e.pid) + R"(,"ts":)" +
               dstd::to_string(e.ts) + R"(,"dur":)" + dstd::to_string(e.dur) +
               "}\n";
    return out + "]\n";
}

dstd::String twin_generic() {
    dstd::String out;
    for (const auto& e : twin_events())
        out += R"({"op":")" + dstd::String(e.name) + R"(","who":)" +
               dstd::to_string(e.pid) + R"(,"at":)" + dstd::to_string(e.ts) +
               R"(,"len":)" + dstd::to_string(e.dur) + "}\n";
    return out;
}

// Events and density blocks of a density body: how many and their summed
// durations, which any bucketing of the same events preserves.
struct DensityTotals {
    double count = 0;
    double total = 0;
};

DensityTotals density_totals(const dstd::String& body) {
    simdjson::dom::parser p;
    auto root = p.parse(simdjson::padded_string(body)).value();
    DensityTotals t;
    for (auto e : root["events"].get_array()) {
        t.count += 1;
        t.total += e["dur"].get_double().value();
    }
    for (auto b : root["density"].get_array()) {
        t.count += b["count"].get_double().value();
        t.total += b["total"].get_double().value();
    }
    return t;
}

// name -> (count, total) of a stats body.
dstd::Map<dstd::String, dstd::Pair<std::uint64_t, double>> stat_rows(
    const dstd::String& body) {
    simdjson::dom::parser p;
    auto root = p.parse(simdjson::padded_string(body)).value();
    dstd::Map<dstd::String, dstd::Pair<std::uint64_t, double>> out;
    for (auto r : root["names"].get_array())
        out[dstd::String(r["name"].get_string().value())] = {
            r["count"].get_uint64().value(), r["total"].get_double().value()};
    return out;
}

}  // namespace

TEST_SUITE("viz_schema") {
    TEST_CASE("a web log serves through its record schema") {
        register_schemas();
        auto s = serve("viz_schema_web", weblog());
        CHECK(s->index->record_schema().id == "srv_weblog");

        simdjson::dom::parser p;
        // Bounds: the time role's zonemap, seconds to microseconds.
        auto info =
            p.parse(simdjson::padded_string(get(*s, "/api/info"))).value();
        const auto t0 = static_cast<std::int64_t>(std::llround(WEB_T0 * 1e6));
        CHECK(info["time_range"]["min_timestamp_us"].get_int64().value() == t0);
        CHECK(info["time_range"]["max_timestamp_us"].get_int64().value() >=
              t0 + (WEB_RECORDS - 1) * 10000);
        CHECK(dstd::String(info["schema"]["decoder"].get_string().value()) ==
              "path");
        CHECK(dstd::String(
                  info["schema"]["fields"]["entity"].get_string().value()) ==
              "client");
        CHECK(dstd::String(
                  info["schema"]["fields"]["label"].get_string().value()) ==
              "req");

        const dstd::String window = "begin=0&end=10000000";
        // Density: every timed record, whether folded or drawn.
        for (const dstd::String& extra :
             {dstd::String(), "&file=" + file_of(*s)}) {
            const auto t = density_totals(
                get(*s, "/api/viz/density?" + window + "&width=200" + extra));
            CHECK(t.count == doctest::Approx(WEB_RECORDS));
        }

        // Events carry the label, a numeric lane id and microsecond times.
        auto events = p.parse(simdjson::padded_string(
                                  get(*s, "/api/viz/events?" + window +
                                              "&summary=1&width=100000")))
                          .value();
        dstd::Set<dstd::String> names;
        std::size_t n = 0;
        for (auto e : events["events"].get_array()) {
            ++n;
            names.insert(dstd::String(e["name"].get_string().value()));
            CHECK(e["pid"].is_int64());
            CHECK(e["dur"].get_int64().value() >= 2000);
        }
        CHECK(n == WEB_RECORDS);
        CHECK(names == dstd::Set<dstd::String>{"GET /a", "POST /b"});

        auto untimed = p.parse(simdjson::padded_string(
                                   get(*s, "/api/viz/untimed?limit=10")))
                           .value();
        CHECK(untimed["count"].get_uint64().value() == WEB_UNTIMED);
        for (auto e : untimed["events"].get_array()) {
            CHECK(dstd::String(e["name"].get_string().value()) == "GET /late");
            CHECK(e["dur"].get_int64().value() == 500000);
        }

        // Analyze: per label and per entity value, summary and live alike.
        for (const dstd::String& extra :
             {dstd::String(), "&file=" + file_of(*s)}) {
            const auto by_name = stat_rows(
                get(*s, "/api/viz/stats?begin=0&end=10000000" + extra));
            REQUIRE(by_name.size() == 2);
            CHECK(by_name.at("GET /a").first == WEB_RECORDS / 2);
            const auto by_pid = stat_rows(get(
                *s, "/api/viz/stats?begin=0&end=10000000&group=pid" + extra));
            CHECK(by_pid.size() == 3);
            CHECK(by_pid.count("10.0.0.1") == 1);
        }

        // Process tree: one node per client, named, on its host.
        auto tree =
            p.parse(simdjson::padded_string(get(*s, "/api/viz/proctree")))
                .value();
        dstd::Map<dstd::String, dstd::String> host_of;
        for (auto node : tree["nodes"].get_array())
            host_of[dstd::String(node["label"].get_string().value())] =
                dstd::String(node["host"].get_string().value());
        CHECK(host_of ==
              dstd::Map<dstd::String, dstd::String>{{"10.0.0.0", "web-0"},
                                                    {"10.0.0.1", "web-1"},
                                                    {"10.0.0.2", "web-2"}});

        // Columns: the record's fields; the time and duration axes are not
        // group options, the entity is.
        auto cols =
            p.parse(simdjson::padded_string(get(*s, "/api/viz/columns")))
                .value();
        dstd::Set<dstd::String> col_names;
        for (auto c : cols["columns"].get_array())
            col_names.insert(dstd::String(c.get_string().value()));
        CHECK(col_names.count("client") == 1);
        CHECK(col_names.count("status") == 1);
        CHECK(col_names.count("t") == 0);
        CHECK(col_names.count("took") == 0);

        // A user filter names the record's own fields.
        const auto filtered = density_totals(
            get(*s, "/api/viz/density?" + window +
                        "&width=200&duql=client%20%3D%3D%20%2210.0.0.1%22"));
        CHECK(filtered.count == doctest::Approx(WEB_RECORDS / 3));
    }

    TEST_CASE("nested nanosecond spans serve through their record schema") {
        register_schemas();
        auto s = serve("viz_schema_otel", otel());
        CHECK(s->index->record_schema().id == "srv_otel");

        simdjson::dom::parser p;
        auto info =
            p.parse(simdjson::padded_string(get(*s, "/api/info"))).value();
        CHECK(info["time_range"]["min_timestamp_us"].get_int64().value() ==
              OTEL_T0 / 1000);

        const auto t = density_totals(
            get(*s, "/api/viz/density?begin=0&end=10000000&width=300"));
        CHECK(t.count == doctest::Approx(OTEL_REQUESTS * 4));
        CHECK(t.total == doctest::Approx(OTEL_REQUESTS * 18500.0));

        auto untimed =
            p.parse(simdjson::padded_string(get(*s, "/api/viz/untimed")))
                .value();
        CHECK(untimed["count"].get_uint64().value() == 0);

        // No hosts row set: every service is a root with no host.
        auto tree_nodes =
            p.parse(simdjson::padded_string(get(*s, "/api/viz/proctree")))
                .value();
        dstd::Set<dstd::String> services;
        for (auto node : tree_nodes["nodes"].get_array()) {
            services.insert(dstd::String(node["label"].get_string().value()));
            CHECK(dstd::String(node["host"].get_string().value()).empty());
            CHECK(node["parent"].get_int64().value() == -1);
        }
        CHECK(services == dstd::Set<dstd::String>{"api", "db"});
    }

    // Needs the containment fold to read a path schema's role and label
    // fields in microseconds; the endpoint passes it the schema's names.
    TEST_CASE("nested nanosecond spans nest into a call tree" *
              doctest::may_fail()) {
        register_schemas();
        auto s = serve("viz_schema_otel_tree", otel());
        simdjson::dom::parser p;
        auto tree = p.parse(simdjson::padded_string(get(
                                *s, "/api/viz/calltree?begin=0&end=10000000")))
                        .value();
        dstd::Map<dstd::String, double> top;
        dstd::Map<dstd::String, double> under_handle;
        for (auto c : tree["tree"]["children"].get_array()) {
            const dstd::String name(c["name"].get_string().value());
            top[name] = c["total"].get_double().value();
            if (name == "handle")
                for (auto k : c["children"].get_array())
                    under_handle[dstd::String(k["name"].get_string().value())] =
                        k["total"].get_double().value();
        }
        CHECK(top["handle"] == doctest::Approx(OTEL_REQUESTS * 10000.0));
        CHECK(top["exec"] == doctest::Approx(OTEL_REQUESTS * 1500.0));
        CHECK(under_handle["db.query"] ==
              doctest::Approx(OTEL_REQUESTS * 2000.0));
        CHECK(under_handle["render"] ==
              doctest::Approx(OTEL_REQUESTS * 5000.0));
    }

    TEST_CASE("the columns endpoint refuses files of two record schemas") {
        register_schemas();
        auto s = dstd::make_unique<Served>("viz_schema_mixed");
        write_gz(s->dir.path() / "a.pfw.gz", twin_dftracer());
        write_gz(s->dir.path() / "b.pfw.gz", twin_generic());
        s->index = dstd::make_unique<TraceIndex>(
            dstd::String(s->dir.str()), dstd::String(s->dir.file("idx")));
        Runtime rt(4);
        auto task = coro::with_scope(
            [&](coro::Scope&) -> coro::CoroTask<dstd::Result<void>> {
                co_return co_await s->index->initialize();
            });
        dftu_utils_test::ok(rt.submit(std::move(task), "setup").wait());
        rt.shutdown();
        REQUIRE(s->index->file_count() == 2);
        domains::server::register_viz_api(s->router, *s->index);

        HttpRequest req;
        req.method = "GET";
        req.path = "/api/viz/columns";
        Runtime rt2(4);
        int status = 0;
        dstd::String body;
        auto get_task =
            coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
                HttpResponse resp = co_await s->router.handle(req);
                status = resp.status_code;
                body = resp.body;
                co_return;
            });
        dftu_utils_test::ok(rt2.submit(std::move(get_task), "get").wait());
        rt2.shutdown();
        CHECK(status == 400);
        CHECK(body.find("one record schema") != dstd::String::npos);
    }

    TEST_CASE("a trace served as dftracer and as generic records agree") {
        register_schemas();
        auto dft = serve("viz_schema_dft", twin_dftracer());
        auto gen = serve("viz_schema_gen", twin_generic());
        CHECK(dft->index->record_schema().id == "dftracer");
        CHECK(gen->index->record_schema().id == "srv_twin");
        CHECK(dft->index->global_min_timestamp_us() ==
              gen->index->global_min_timestamp_us());

        const dstd::String window = "begin=0&end=10000000";
        for (const dstd::String& extra :
             {dstd::String(), dstd::String("&file=")}) {
            auto target = [&](Served& s) {
                return "/api/viz/density?" + window + "&width=250" + extra +
                       (extra.empty() ? "" : file_of(s));
            };
            const auto a = density_totals(get(*dft, target(*dft)));
            const auto b = density_totals(get(*gen, target(*gen)));
            CHECK(a.count ==
                  doctest::Approx(static_cast<double>(twin_events().size())));
            CHECK(b.count == doctest::Approx(a.count));
            CHECK(b.total == doctest::Approx(a.total));
        }

        for (const dstd::String& extra :
             {dstd::String(), dstd::String("&file=")}) {
            auto target = [&](Served& s) {
                return "/api/viz/stats?" + window + extra +
                       (extra.empty() ? "" : file_of(s));
            };
            const auto a = stat_rows(get(*dft, target(*dft)));
            const auto b = stat_rows(get(*gen, target(*gen)));
            CHECK(a.size() == 3);
            CHECK(a == b);
        }

        const dstd::String tree = get(*dft, "/api/viz/calltree?" + window);
        CHECK(tree.find(R"("name":"read","total":120000.0)") !=
              dstd::String::npos);
        CHECK(tree == get(*gen, "/api/viz/calltree?" + window));
        CHECK(get(*dft, "/api/viz/calltree?" + window + "&group=pid") ==
              get(*gen, "/api/viz/calltree?" + window + "&group=pid"));
        CHECK(get(*dft, "/api/viz/histogram?" + window) ==
              get(*gen, "/api/viz/histogram?" + window));
    }
}
