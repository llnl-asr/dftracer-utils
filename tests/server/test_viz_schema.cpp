#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/coro/coro.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/trace_api.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/server/viz_api.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_rechunker.h>
#include <doctest/doctest.h>
#include <simdjson.h>
#include <testing_utilities.h>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace dftracer::utils;
using dftracer::utils::server::HttpRequest;
using dftracer::utils::server::HttpResponse;
using dftracer::utils::server::Router;
using dftracer::utils::server::TraceIndex;

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
        index::register_schema(spec, "test_viz_schema");
}

void write_gz(const fs::path& path, const std::string& content) {
    using dftracer::utils::utilities::fileio::compress::GzipMemberCompressor;
    GzipMemberCompressor comp;
    auto member = comp.compress_member(content.data(), content.size());
    REQUIRE(member.has_value());
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(member->data()),
              static_cast<std::streamsize>(member->size()));
}

struct Served {
    std::unique_ptr<TraceIndex> index;
    Router router;
};

// A server over `content`, written as the one trace file of a fresh
// directory. Discovery lists .pfw.gz files only.
std::unique_ptr<Served> serve(const std::string& name,
                              const std::string& content) {
    auto dir = dftu_utils_test::make_unique_test_path(name);
    fs::create_directories(dir);
    write_gz(dir / "trace.pfw.gz", content);
    auto s = std::make_unique<Served>();
    s->index =
        std::make_unique<TraceIndex>(dir.string(), (dir / "idx").string());
    Runtime rt(4);
    auto task =
        run_coro_scope(rt.executor(), [&](CoroScope&) -> coro::CoroTask<void> {
            co_await s->index->initialize();
        });
    rt.submit(std::move(task), "setup").wait();
    rt.shutdown();
    REQUIRE(s->index->file_count() == 1);
    server::register_trace_api(s->router, *s->index);
    server::register_viz_api(s->router, *s->index);
    return s;
}

std::string get(Served& s, const std::string& target) {
    HttpRequest req;
    req.method = "GET";
    req.path = target;
    std::string body;
    Runtime rt(4);
    auto task =
        run_coro_scope(rt.executor(), [&](CoroScope&) -> coro::CoroTask<void> {
            HttpResponse resp = co_await s.router.handle(req);
            CHECK_MESSAGE(resp.status_code == 200, target, ": ", resp.body);
            body = resp.body;
            co_return;
        });
    rt.submit(std::move(task), "get").wait();
    rt.shutdown();
    return body;
}

std::string file_of(Served& s) { return s.index->files().front().path; }

constexpr int WEB_RECORDS = 300;
constexpr int WEB_UNTIMED = 3;
constexpr double WEB_T0 = 1700000000.5;

std::string weblog() {
    std::string out;
    for (int i = 0; i < WEB_RECORDS; ++i) {
        out += R"({"t":)" + std::to_string(WEB_T0 + i * 0.01);
        out += R"(,"took":)" + std::to_string(0.002 + (i % 5) * 0.001);
        out += R"(,"client":"10.0.0.)" + std::to_string(i % 3) + '"';
        out += R"(,"req":")" + std::string(i % 2 ? "GET /a" : "POST /b") + '"';
        out +=
            R"(,"status":200,"host":"web-)" + std::to_string(i % 3) + "\"}\n";
    }
    for (int i = 0; i < WEB_UNTIMED; ++i)
        out += R"({"took":0.5,"client":"10.0.0.9","req":"GET /late",)"
               R"("status":504,"host":"web-9"})"
               "\n";
    return out;
}

constexpr int OTEL_REQUESTS = 50;
constexpr std::int64_t OTEL_T0 = 1700000000000000000;

std::string otel() {
    std::string out;
    auto span = [&](const char* name, const char* service, std::int64_t start,
                    std::int64_t dur) {
        out += R"({"name":")" + std::string(name) + R"(","span":{"start_ns":)" +
               std::to_string(start) + R"(,"dur_ns":)" + std::to_string(dur) +
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

std::vector<TwinEvent> twin_events() {
    std::vector<TwinEvent> out;
    for (std::int64_t pid : {101, 102})
        for (std::int64_t i = 0; i < 200; ++i) {
            const std::int64_t t = 1'000'000 + i * 1000 + (pid - 101) * 7;
            out.push_back({"open", pid, t, 800});
            out.push_back({"read", pid, t + 100, 300});
            out.push_back({"write", pid, t + 450, 200});
        }
    return out;
}

std::string twin_dftracer() {
    std::string out = "[\n";
    for (const auto& e : twin_events())
        out += R"({"name":")" + std::string(e.name) +
               R"(","cat":"POSIX","ph":1,"pid":)" + std::to_string(e.pid) +
               R"(,"tid":)" + std::to_string(e.pid) + R"(,"ts":)" +
               std::to_string(e.ts) + R"(,"dur":)" + std::to_string(e.dur) +
               "}\n";
    return out + "]\n";
}

std::string twin_generic() {
    std::string out;
    for (const auto& e : twin_events())
        out += R"({"op":")" + std::string(e.name) + R"(","who":)" +
               std::to_string(e.pid) + R"(,"at":)" + std::to_string(e.ts) +
               R"(,"len":)" + std::to_string(e.dur) + "}\n";
    return out;
}

// Events and density blocks of a density body: how many and their summed
// durations, which any bucketing of the same events preserves.
struct DensityTotals {
    double count = 0;
    double total = 0;
};

DensityTotals density_totals(const std::string& body) {
    simdjson::dom::parser p;
    auto root = p.parse(body).value();
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
std::map<std::string, std::pair<std::uint64_t, double>> stat_rows(
    const std::string& body) {
    simdjson::dom::parser p;
    auto root = p.parse(body).value();
    std::map<std::string, std::pair<std::uint64_t, double>> out;
    for (auto r : root["names"].get_array())
        out[std::string(r["name"].get_string().value())] = {
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
        auto info = p.parse(get(*s, "/api/info")).value();
        const auto t0 = static_cast<std::int64_t>(std::llround(WEB_T0 * 1e6));
        CHECK(info["time_range"]["min_timestamp_us"].get_int64().value() == t0);
        CHECK(info["time_range"]["max_timestamp_us"].get_int64().value() >=
              t0 + (WEB_RECORDS - 1) * 10000);
        CHECK(std::string(info["schema"]["decoder"].get_string().value()) ==
              "path");
        CHECK(std::string(
                  info["schema"]["fields"]["entity"].get_string().value()) ==
              "client");
        CHECK(std::string(
                  info["schema"]["fields"]["label"].get_string().value()) ==
              "req");

        const std::string window = "begin=0&end=10000000";
        // Density: every timed record, whether folded or drawn.
        for (const std::string& extra :
             {std::string(), "&file=" + file_of(*s)}) {
            const auto t = density_totals(
                get(*s, "/api/viz/density?" + window + "&width=200" + extra));
            CHECK(t.count == doctest::Approx(WEB_RECORDS));
        }

        // Events carry the label, a numeric lane id and microsecond times.
        auto events = p.parse(get(*s, "/api/viz/events?" + window +
                                          "&summary=1&width=100000"))
                          .value();
        std::set<std::string> names;
        std::size_t n = 0;
        for (auto e : events["events"].get_array()) {
            ++n;
            names.insert(std::string(e["name"].get_string().value()));
            CHECK(e["pid"].is_int64());
            CHECK(e["dur"].get_int64().value() >= 2000);
        }
        CHECK(n == WEB_RECORDS);
        CHECK(names == std::set<std::string>{"GET /a", "POST /b"});

        auto untimed = p.parse(get(*s, "/api/viz/untimed?limit=10")).value();
        CHECK(untimed["count"].get_uint64().value() == WEB_UNTIMED);
        for (auto e : untimed["events"].get_array()) {
            CHECK(std::string(e["name"].get_string().value()) == "GET /late");
            CHECK(e["dur"].get_int64().value() == 500000);
        }

        // Analyze: per label and per entity value, summary and live alike.
        for (const std::string& extra :
             {std::string(), "&file=" + file_of(*s)}) {
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
        auto tree = p.parse(get(*s, "/api/viz/proctree")).value();
        std::map<std::string, std::string> host_of;
        for (auto node : tree["nodes"].get_array())
            host_of[std::string(node["label"].get_string().value())] =
                std::string(node["host"].get_string().value());
        CHECK(host_of ==
              std::map<std::string, std::string>{{"10.0.0.0", "web-0"},
                                                 {"10.0.0.1", "web-1"},
                                                 {"10.0.0.2", "web-2"}});

        // Columns: the record's fields; the time and duration axes are not
        // group options, the entity is.
        auto cols = p.parse(get(*s, "/api/viz/columns")).value();
        std::set<std::string> col_names;
        for (auto c : cols["columns"].get_array())
            col_names.insert(std::string(c.get_string().value()));
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
        auto info = p.parse(get(*s, "/api/info")).value();
        CHECK(info["time_range"]["min_timestamp_us"].get_int64().value() ==
              OTEL_T0 / 1000);

        const auto t = density_totals(
            get(*s, "/api/viz/density?begin=0&end=10000000&width=300"));
        CHECK(t.count == doctest::Approx(OTEL_REQUESTS * 4));
        CHECK(t.total == doctest::Approx(OTEL_REQUESTS * 18500.0));

        auto untimed = p.parse(get(*s, "/api/viz/untimed")).value();
        CHECK(untimed["count"].get_uint64().value() == 0);

        // No hosts row set: every service is a root with no host.
        auto tree_nodes = p.parse(get(*s, "/api/viz/proctree")).value();
        std::set<std::string> services;
        for (auto node : tree_nodes["nodes"].get_array()) {
            services.insert(std::string(node["label"].get_string().value()));
            CHECK(std::string(node["host"].get_string().value()).empty());
            CHECK(node["parent"].get_int64().value() == -1);
        }
        CHECK(services == std::set<std::string>{"api", "db"});
    }

    // Needs the containment fold to read a path schema's role and label
    // fields in microseconds; the endpoint passes it the schema's names.
    TEST_CASE("nested nanosecond spans nest into a call tree" *
              doctest::may_fail()) {
        register_schemas();
        auto s = serve("viz_schema_otel_tree", otel());
        simdjson::dom::parser p;
        auto tree =
            p.parse(get(*s, "/api/viz/calltree?begin=0&end=10000000")).value();
        std::map<std::string, double> top;
        std::map<std::string, double> under_handle;
        for (auto c : tree["tree"]["children"].get_array()) {
            const std::string name(c["name"].get_string().value());
            top[name] = c["total"].get_double().value();
            if (name == "handle")
                for (auto k : c["children"].get_array())
                    under_handle[std::string(k["name"].get_string().value())] =
                        k["total"].get_double().value();
        }
        CHECK(top["handle"] == doctest::Approx(OTEL_REQUESTS * 10000.0));
        CHECK(top["exec"] == doctest::Approx(OTEL_REQUESTS * 1500.0));
        CHECK(under_handle["db.query"] ==
              doctest::Approx(OTEL_REQUESTS * 2000.0));
        CHECK(under_handle["render"] ==
              doctest::Approx(OTEL_REQUESTS * 5000.0));
    }

    TEST_CASE("a trace served as dftracer and as generic records agree") {
        register_schemas();
        auto dft = serve("viz_schema_dft", twin_dftracer());
        auto gen = serve("viz_schema_gen", twin_generic());
        CHECK(dft->index->record_schema().id == "dftracer");
        CHECK(gen->index->record_schema().id == "srv_twin");
        CHECK(dft->index->global_min_timestamp_us() ==
              gen->index->global_min_timestamp_us());

        const std::string window = "begin=0&end=10000000";
        for (const std::string& extra :
             {std::string(), std::string("&file=")}) {
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

        for (const std::string& extra :
             {std::string(), std::string("&file=")}) {
            auto target = [&](Served& s) {
                return "/api/viz/stats?" + window + extra +
                       (extra.empty() ? "" : file_of(s));
            };
            const auto a = stat_rows(get(*dft, target(*dft)));
            const auto b = stat_rows(get(*gen, target(*gen)));
            CHECK(a.size() == 3);
            CHECK(a == b);
        }

        const std::string tree = get(*dft, "/api/viz/calltree?" + window);
        CHECK(tree.find(R"("name":"read","total":120000.0)") !=
              std::string::npos);
        CHECK(tree == get(*gen, "/api/viz/calltree?" + window));
        CHECK(get(*dft, "/api/viz/calltree?" + window + "&group=pid") ==
              get(*gen, "/api/viz/calltree?" + window + "&group=pid"));
        CHECK(get(*dft, "/api/viz/histogram?" + window) ==
              get(*gen, "/api/viz/histogram?" + window));
    }
}
