#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

using namespace dftracer::utils;
namespace ix = dftracer::utils::index;

namespace {

std::string error_of(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "no error";
}

bool mentions(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// nginx-style JSON logs over small gzip members: `upstream` is constant for
// runs of 300 records, so chunks hold disjoint upstreams.
std::string write_nginx(const std::string& dir, int records) {
    std::ostringstream out;
    for (int i = 0; i < records; ++i)
        out << R"({"remote_addr":"10.0.0.)" << i % 7 << R"(","status":)"
            << (i % 11 == 0 ? 404 : 200) << R"(,"request_time":)" << i % 13
            << R"(.25,"upstream":"u)" << i / 300 << R"(","path":"/p)" << i % 5
            << "\"}\n";
    const auto plain = dir + "/access.ndjson";
    {
        std::ofstream(plain) << out.str();
    }
    const auto gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               8 * 1024));
    fs::remove(plain);
    return gz;
}

std::size_t count_rows(const std::string& trace, const std::string& index_dir,
                       const std::string& query, bool skip_pruning) {
    auto run = [&]() -> coro::CoroTask<std::size_t> {
        utilities::reader::TraceReader reader(
            {.file_path = trace, .index_dir = index_dir});
        utilities::reader::ReadConfig cfg;
        cfg.query = query;
        cfg.skip_pruning = skip_pruning;
        auto gen = reader.read_json(cfg);
        std::size_t n = 0;
        while (auto line = co_await gen.next()) ++n;
        co_return n;
    };
    return run().get();
}

}  // namespace

TEST_SUITE("SchemaRegistry") {
    TEST_CASE("a YAML spec declares typed fields and derives the rest") {
        const auto& s = ix::register_schema(
            "id: yaml_a\n"
            "# Fields.\n"
            "fields:\n"
            "  status: {type: int}\n"
            "  request_time: {type: float, role: duration, unit: s}\n"
            "  ts_ms: {type: int, role: time, unit: ms, optional: true}\n"
            "  host: {type: string, path: meta.host, optional: true}\n"
            "  upstream: {type: string, always_index: true}\n"
            "index: {path_budget: 8}\n",
            "yaml_a.yaml");
        CHECK(s.id == "yaml_a");
        CHECK(s.decoder == ix::Decoder::PATH);
        CHECK(s.require ==
              std::vector<std::string>{"status", "request_time", "upstream"});
        CHECK(s.roles.time == "ts_ms");
        CHECK(s.roles.time_unit == ix::TimeUnit::MS);
        CHECK(s.roles.duration == "request_time");
        CHECK(s.roles.duration_unit == ix::TimeUnit::S);
        CHECK(s.always_index == std::vector<std::string>{"ts_ms", "upstream"});
        REQUIRE(s.field_at("meta.host") != nullptr);
        CHECK(s.field_at("meta.host")->name == "host");
        CHECK(s.path_budget == 8);
        CHECK(&ix::get_schema("yaml_a") == &s);
        CHECK(s.params_hash() != ix::get_schema("generic").params_hash());
    }

    TEST_CASE("a JSON spec and a SchemaSpec reach the same schema") {
        const auto& s = ix::register_schema(
            R"({"id":"json_a","fields":{"status":{"type":"int"},)"
            R"("upstream":{"type":"string","always_index":true}}})",
            "json_a.json");
        ix::SchemaSpec spec;
        spec.id = "json_a";
        ix::FieldSpec upstream;
        upstream.name = "upstream";
        upstream.always_index = true;
        ix::FieldSpec status;
        status.name = "status";
        status.type = ix::FieldType::INT;
        // Field order does not change the definition.
        spec.fields = {upstream, status};
        CHECK(&ix::register_schema(spec, "code") == &s);
        CHECK_FALSE(s.path_budget);
    }

    TEST_CASE("extending dftracer keeps its decoder, roles and dictionaries") {
        const auto& s = ix::register_schema(
            "id: ext_dft\nextends: dftracer\n"
            "fields: {step: {type: int, path: args.step}}\n"
            "dictionaries:\n"
            "  - {name: host, rows: HH, key: args.value,\n"
            "     fields: {name: args.name, fqdn: args.fqdn}, keys_in: "
            "[hhash]}\n",
            "ext_dft.yaml");
        const auto& dft = ix::get_schema("dftracer");
        CHECK(s.decoder == ix::Decoder::DFTRACER);
        CHECK(s.require == std::vector<std::string>{"ph", "name", "args.step"});
        CHECK(s.roles.time == dft.roles.time);
        CHECK(s.roles.phase == "ph");
        CHECK(s.always_index.empty());
        REQUIRE(s.dictionaries.size() == dft.dictionaries.size());
        CHECK(s.dictionary_of("hhash")->has_field("fqdn"));
        CHECK(s.dictionary_of("fhash")->has_field("path"));
    }

    TEST_CASE("a child field of the same name replaces the parent's") {
        ix::register_schema(
            "id: base_a\nfields: {lat_a: {type: float}, op_a: {type: "
            "string}}\n",
            "base.yaml");
        const auto& s = ix::register_schema(
            "id: child_a\nextends: base_a\n"
            "fields: {lat_a: {type: float, optional: true}}\n",
            "child.yaml");
        CHECK(s.require == std::vector<std::string>{"op_a"});
        CHECK(s.fields.size() == 2);
    }

    TEST_CASE("malformed specs name the source and the key") {
        auto reject = [](const char* text, const char* part) {
            const auto what =
                error_of([&] { ix::register_schema(text, "bad.yaml"); });
            CAPTURE(text);
            CAPTURE(what);
            CHECK(mentions(what, "bad.yaml"));
            CHECK(mentions(what, part));
        };
        reject("id: bad_a\nfileds: {x: {type: int}}\n", "fileds");
        reject("id: bad_a\ndetect: {require: [x]}\n", "detect");
        reject("id: bad_a\nroles: {time: t}\n", "roles");
        reject("id: bad_a\nindex: {pin: [x]}\n", "index.pin");
        reject("fields: {x: {type: int}}\n", "missing id");
        reject("id: bad_b\nfields: {x: {path: y}}\n", "fields.x.type");
        reject("id: bad_b\nfields: {x: {type: long}}\n", "fields.x.type");
        reject("id: bad_b\nfields: {x: {type: int, colour: red}}\n",
               "fields.x.colour");
        reject("id: bad_b\nfields: {x: {type: int, unit: ms}}\n",
               "fields.x.unit");
        reject("id: bad_b\nfields: {x: {type: string, role: time}}\n",
               "fields.x");
        reject("id: bad_b\nfields: {x: {type: int, role: clock}}\n",
               "fields.x.role");
        reject("id: bad_b\nfields: {x: {type: int, optional: maybe}}\n",
               "fields.x.optional");
        reject(
            "id: bad_b\nfields: {a: {type: int, role: time},\n"
            "  b: {type: int, role: time}}\n",
            "time role");
        reject("id: bad_b\nfields: {a: {type: int}, b: {type: int, path: a}}\n",
               "share the path a");
        reject("id: bad_c\nextends: nope\n", "unknown extends nope");
        reject("id: bad_d\nextends: bad_d\n", "extends itself");
        reject("id: bad_e\nindex: {path_budget: many}\n", "index.path_budget");
        reject(
            "id: bad_g\ndictionaries:\n  - {name: d, rows: R, key: k, "
            "fields: {[a]: b}}\n",
            "dictionaries.fields");
        reject("id: [unclosed\n", "bad.yaml");
        reject("id: generic\n", "cannot be redefined");
        CHECK(ix::find_schema("bad_a") == nullptr);
        CHECK(ix::find_schema("bad_b") == nullptr);
    }

    TEST_CASE("one id has one definition") {
        const auto& first = ix::register_schema(
            "id: twice\nfields: {a: {type: int}}\n", "one.yaml");
        CHECK(&ix::register_schema("id: twice\nfields: {a: {type: int}}\n",
                                   "again.yaml") == &first);
        const auto what = error_of([] {
            ix::register_schema("id: twice\nfields: {a: {type: float}}\n",
                                "two.yaml");
        });
        CHECK(mentions(what, "one.yaml"));
        CHECK(mentions(what, "two.yaml"));
    }

    TEST_CASE("an unknown id lists the registered ones and where they load") {
        const auto what = error_of([] { ix::get_schema("no_such"); });
        CHECK(mentions(what, "no_such"));
        CHECK(mentions(what, "dftracer"));
        CHECK(mentions(what, "DFTRACER_SCHEMA_PATH"));
        CHECK(mentions(what, "<index_dir>/schemas/"));
    }

    TEST_CASE("the built-ins are declared with fields") {
        const auto& dft = ix::get_schema("dftracer");
        CHECK(dft.builtin);
        CHECK(dft.require == std::vector<std::string>{"ph", "name"});
        CHECK(dft.roles.time == "ts");
        CHECK(dft.roles.duration == "dur");
        CHECK(dft.roles.entity == "pid");
        CHECK(dft.roles.phase == "ph");
        CHECK(dft.always_index.empty());
        const auto& generic = ix::get_schema("generic");
        CHECK(generic.fields.empty());
        CHECK(generic.require.empty());
    }

    TEST_CASE("the genesis built-in extends dftracer with a run dictionary") {
        const auto& g = ix::get_schema("genesis");
        CHECK(g.builtin);
        CHECK(g.decoder == ix::Decoder::DFTRACER);
        CHECK(g.require == std::vector<std::string>{"ph", "name", "args.run",
                                                    "args.path", "args.depth",
                                                    "args.count"});
        const auto* run = g.dictionary_of("run");
        REQUIRE(run != nullptr);
        CHECK(run->rows == "RUN");
        CHECK(run->key == "args.run");
        CHECK(run->has_field("papi_set"));
        CHECK(run->has_field("nodes"));
        CHECK(g.dictionary_of("fhash") != nullptr);
        CHECK(g.resolved_column("resolved.run.app").field == "app");

        std::vector<std::string_view> lines = {
            R"({"id":0,"name":"RUN","cat":"genesis","pid":0,"tid":0,"ph":4,)"
            R"("args":{"run":"ab","app":"laghos","nodes":1}})"};
        for (int i = 0; i < 12; ++i)
            lines.push_back(
                R"({"id":1,"name":"f","cat":"c","pid":0,"tid":0,"ph":3,"ts":1,)"
                R"("args":{"run":"ab","path":"main;f","depth":1,"count":2}})");
        CHECK(ix::detect_schema(lines).id == "genesis");
        const std::vector<std::string_view> dft = {
            R"({"id":1,"name":"read","cat":"POSIX","ph":"X","ts":1})"};
        CHECK(ix::detect_schema(dft).id == "dftracer");
    }

    TEST_CASE("a directory of specs loads once") {
        dftu_utils_test::TestEnvironment env(1);
        const auto dir = env.get_dir() + "/specs";
        fs::create_directories(dir);
        std::ofstream(dir + "/a.yaml") << "id: dir_a\n";
        std::ofstream(dir + "/b.json") << R"({"id":"dir_b","extends":"dir_a"})";
        std::ofstream(dir + "/notes.txt") << "not a spec";
        ix::load_schemas(dir);
        REQUIRE(ix::find_schema("dir_b") != nullptr);
        ix::load_schemas(dir);

        const auto bad = env.get_dir() + "/broken";
        fs::create_directories(bad);
        std::ofstream(bad + "/c.yaml") << "id: dir_c\nbogus: 1\n";
        const auto what = error_of([&] { ix::load_schemas(bad); });
        CHECK(mentions(what, "c.yaml"));
        CHECK(mentions(what, "bogus"));
    }

    TEST_CASE("detection picks a user schema and never one requiring nothing") {
        ix::register_schema(
            "id: det_nginx\n"
            "fields: {status: {type: int}, request_time: {type: float}}\n",
            "det.yaml");
        ix::register_schema("id: aaa_empty\n", "empty.yaml");
        const std::vector<std::string_view> nginx = {
            R"({"status":200,"request_time":0.5})",
            R"({"status":404,"request_time":1.5,"x":1})"};
        const std::vector<std::string_view> plain = {R"({"op":"read"})"};
        CHECK(ix::detect_schema(nginx).id == "det_nginx");
        CHECK(ix::detect_schema(plain).id == "generic");
    }

    TEST_CASE("explain reports each schema's share and the choice") {
        ix::register_schema(
            "id: exp_nginx\n"
            "fields: {status: {type: int}, request_time: {type: float}}\n",
            "exp.yaml");
        std::vector<std::string_view> lines;
        for (int i = 0; i < 8; ++i)
            lines.push_back(R"({"status":200,"request_time":0.5})");
        lines.push_back(R"({"status":200})");
        lines.push_back(R"({"op":"read"})");
        const auto d = ix::explain_schema(lines);
        CHECK(d.objects == 10);
        CHECK(d.chosen->id == "generic");
        const auto it = std::find_if(d.scores.begin(), d.scores.end(),
                                     [](const ix::SchemaScore& s) {
                                         return s.schema->id == "exp_nginx";
                                     });
        REQUIRE(it != d.scores.end());
        CHECK(it->share == doctest::Approx(0.8));
    }

    TEST_CASE("a user schema from the index directory is detected and used") {
        dftu_utils_test::TestEnvironment env(1);
        const auto gz = write_nginx(env.get_dir(), 3000);
        const auto index_dir = env.get_dir() + "/idx";
        fs::create_directories(index_dir + "/schemas");
        // path_budget 0: only the always-indexed field is indexed.
        std::ofstream(index_dir + "/schemas/nginx.yaml")
            << "id: idx_nginx\n"
               "fields:\n"
               "  remote_addr: {type: string}\n"
               "  status: {type: int}\n"
               "  request_time: {type: float}\n"
               "  upstream: {type: string, optional: true, always_index: "
               "true}\n"
               "index: {path_budget: 0}\n";
        ix::IndexerOptions o;
        o.index_dir = index_dir;
        auto indexer = ix::Indexer::open({gz}, o);
        indexer.build();
        const auto files = indexer.files();
        REQUIRE(files.size() == 1);
        CHECK(files[0].schema == "idx_nginx");

        const auto e = indexer.explain(R"(upstream == "u3")");
        REQUIRE(e.size() == 1);
        CHECK(e[0].read.size() < e[0].chunks);
        const std::string q = R"(upstream == "u3")";
        CHECK(count_rows(gz, index_dir, q, false) == 300);
        CHECK(count_rows(gz, index_dir, q, true) == 300);
        // Not always indexed and past the budget: correct but not pruned.
        const auto s = indexer.explain("status == 404");
        CHECK(s[0].read.size() == s[0].chunks);
        CHECK(count_rows(gz, index_dir, "status == 404", false) ==
              count_rows(gz, index_dir, "status == 404", true));
        // A second build finds the file current.
        CHECK(indexer.build().indexed == 0);
    }
}
