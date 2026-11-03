// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/chunk_indexer.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/source.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <fstream>
#include <sstream>

using namespace dftracer::utils;
namespace ix = dftracer::utils::index;

namespace {

dstd::String error_of(const dstd::Function<void()>& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "no error";
}

bool mentions(const dstd::String& text, const dstd::String& part) {
    return text.find(part) != dstd::String::npos;
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
        std::ofstream(plain.c_str()) << out.str();
    }
    const auto gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               8 * 1024));
    (void)dstd::fs::remove(plain);
    return gz;
}

std::size_t count_rows(dstd::StringView trace, dstd::StringView index_dir,
                       const dstd::String& query, bool skip_pruning) {
    auto run = [&]() -> coro::CoroTask<std::size_t> {
        utilities::reader::TraceReader reader(
            {.file_path = dstd::String(trace),
             .index_dir = dstd::String(index_dir)});
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
        CHECK(s.decoder == ix::Decoder::Path);
        CHECK(s.require ==
              dstd::Vector<dstd::String>{"status", "request_time", "upstream"});
        CHECK(s.roles.time == "ts_ms");
        CHECK(s.roles.time_unit == ix::TimeUnit::Ms);
        CHECK(s.roles.duration == "request_time");
        CHECK(s.roles.duration_unit == ix::TimeUnit::S);
        CHECK(s.always_index ==
              dstd::Vector<dstd::String>{"request_time", "ts_ms", "upstream"});
        REQUIRE(s.field_at("meta.host") != nullptr);
        CHECK(s.field_at("meta.host")->name == "host");
        CHECK(s.path_budget == 8);
        CHECK(&ix::get_schema("yaml_a") == &s);
        CHECK(s.params_hash() != ix::get_schema("generic").params_hash());
    }

    TEST_CASE(
        "index.never_index is derived, hashed and refused where it "
        "conflicts") {
        const auto& s = ix::register_schema(
            "id: yaml_never\n"
            "fields:\n"
            "  zz_nv_status: {type: int}\n"
            "index: {never_index: [zz_nv_z, zz_nv_blob, zz_nv_z]}\n",
            "yaml_never.yaml");
        CHECK(s.never_index ==
              dstd::Vector<dstd::String>{"zz_nv_blob", "zz_nv_z"});
        CHECK(ix::schemas_json().find("\"never_index\":[\"zz_nv_blob\"") !=
              dstd::String::npos);
        auto cfg = ix::build::ChunkIndexerConfig{};
        cfg.extra_dimensions = {"zz_nv_blob", "zz_nv_status"};
        CHECK(ix::build::for_schema(cfg, s).extra_dimensions ==
              dstd::Vector<dstd::String>{"zz_nv_status"});

        ix::RecordSchema fewer = s;
        fewer.never_index.pop_back();
        CHECK(fewer.params_hash() != s.params_hash());

        const auto& child = ix::register_schema(
            "id: yaml_never_child\nextends: yaml_never\n"
            "index: {never_index: [zz_nv_a]}\n",
            "yaml_never_child.yaml");
        CHECK(child.never_index ==
              dstd::Vector<dstd::String>{"zz_nv_a", "zz_nv_blob", "zz_nv_z"});

        CHECK(mentions(error_of([] {
                           ix::register_schema(
                               "id: never_role\nfields: {x: {type: int, role: "
                               "entity}}\nindex: {never_index: [x]}\n",
                               "never_role.yaml");
                       }),
                       "entity role"));
        CHECK(mentions(
            error_of([] {
                ix::register_schema(
                    "id: never_always\nfields: {x: {type: string, "
                    "always_index: true}}\nindex: {never_index: [x]}\n",
                    "never_always.yaml");
            }),
            "always_index"));
        CHECK(mentions(error_of([] {
                           ix::register_schema(
                               "id: never_dft\nextends: dftracer\n"
                               "index: {never_index: [args.x]}\n",
                               "never_dft.yaml");
                       }),
                       "decodes records by path"));
        CHECK(mentions(error_of([] {
                           ix::register_schema(
                               "id: never_text\nindex: {never_index: abc}\n",
                               "never_text.yaml");
                       }),
                       "list of paths"));
        CHECK(
            mentions(error_of([] {
                         ix::register_schema(
                             "id: never_empty\nindex: {never_index: [\"\"]}\n",
                             "never_empty.yaml");
                     }),
                     "list of paths"));
        CHECK(mentions(error_of([] {
                           ix::register_schema(
                               "id: never_rowset\nfields: {y: {type: int}}\n"
                               "index: {never_index: [x]}\n"
                               "source: |\n  xs = where y > 0 | select x\n",
                               "never_rowset.yaml");
                       }),
                       "never_index"));
    }

    TEST_CASE("the genesis built-in never indexes its sketches") {
        const auto& g = ix::get_schema("genesis");
        CHECK(g.never_index ==
              dstd::Vector<dstd::String>{"dur.sketch", "v.sketch"});
    }

    TEST_CASE("index.stats_share and index.path_budget override the build") {
        const auto& s = ix::register_schema(
            "id: yaml_share\nfields: {zz_probe_a: {type: int}}\n"
            "index: {stats_share: 0.2, path_budget: 50}\n",
            "yaml_share.yaml");
        REQUIRE(s.stats_share);
        CHECK(*s.stats_share == doctest::Approx(0.2));
        CHECK(s.path_budget == 50);
        const auto cfg =
            ix::build::for_schema(ix::build::ChunkIndexerConfig{}, s);
        CHECK(cfg.stats_share == doctest::Approx(0.2));
        CHECK(cfg.path_budget == 50);
        CHECK(ix::schemas_json().find("\"stats_share\":0.2") !=
              dstd::String::npos);

        const auto& unset = ix::register_schema(
            "id: yaml_share_unset\nfields: {zz_probe_b: {type: int}}\n",
            "yaml_share_unset.yaml");
        CHECK_FALSE(unset.stats_share);
        CHECK(ix::build::for_schema(ix::build::ChunkIndexerConfig{}, unset)
                  .stats_share ==
              doctest::Approx(ix::build::ChunkIndexerConfig{}.stats_share));
        CHECK(s.params_hash() != unset.params_hash());

        ix::RecordSchema other = s;
        other.stats_share = 0.3;
        CHECK(other.params_hash() != s.params_hash());
    }

    TEST_CASE("a built-in's hash changes only when it sets an override") {
        const auto& dft = ix::get_schema("dftracer");
        REQUIRE(dft.builtin);
        CHECK_FALSE(dft.stats_share);
        CHECK_FALSE(dft.path_budget);
        ix::RecordSchema with_share = dft;
        with_share.stats_share = 0.1;
        ix::RecordSchema with_budget = dft;
        with_budget.path_budget = 10;
        CHECK(with_share.params_hash() != dft.params_hash());
        CHECK(with_budget.params_hash() != dft.params_hash());
        CHECK(with_share.params_hash() != with_budget.params_hash());
        with_share.stats_share.reset();
        CHECK(with_share.params_hash() == dft.params_hash());
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
        status.type = ix::FieldType::Int;
        // Field order does not change the definition.
        spec.fields = {upstream, status};
        CHECK(&ix::register_schema(spec, "code") == &s);
        CHECK_FALSE(s.path_budget);
    }

    TEST_CASE("extending dftracer keeps its decoder, roles and source") {
        const auto& s = ix::register_schema(
            "id: ext_dft\nextends: dftracer\n"
            "fields: {step: {type: int, path: args.step}}\n"
            "source: |\n"
            "  hosts = where name == \"HH\" | select hhash = args.value, "
            "fqdn = args.fqdn;\n"
            "  steps = where step > 0\n",
            "ext_dft.yaml");
        const auto& dft = ix::get_schema("dftracer");
        CHECK(s.decoder == ix::Decoder::Dftracer);
        CHECK(s.require ==
              dstd::Vector<dstd::String>{"ph", "name", "ts", "args.step"});
        CHECK(s.roles.time == dft.roles.time);
        CHECK(s.roles.phase == "ph");
        CHECK(s.always_index.empty());
        // The child's hosts replaces the parent's in place; files stays.
        CHECK(mentions(s.source, "hosts = where name == \"HH\""));
        CHECK(mentions(s.source, "fqdn = args.fqdn"));
        CHECK(mentions(s.source, "files = "));
        CHECK(s.source.find("files = ") < s.source.find("hosts = "));
        CHECK(mentions(s.source, "steps = where step > 0"));
        CHECK(s.params_hash() != dft.params_hash());
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
        CHECK(s.require == dstd::Vector<dstd::String>{"op_a"});
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
        reject("id: bad_b\nfields: {x: {type: string, role: duration}}\n",
               "fields.x");
        reject("id: bad_b\nfields: {x: {type: string, role: time, unit: ms}}\n",
               "ISO-8601");
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
        reject("id: bad_e\nindex: {stats_share: 0}\n", "index.stats_share");
        reject("id: bad_e\nindex: {stats_share: 1.5}\n", "index.stats_share");
        reject("id: bad_e\nindex: {stats_share: lots}\n", "index.stats_share");
        reject("id: bad_g\ndictionaries:\n  - {name: d, rows: R, key: k}\n",
               "unknown key dictionaries");
        reject("id: bad_h\nsource: \"a = where x >\"\n", "bad_h");
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
        CHECK(mentions(what, "DFTRACER_UTILS_SCHEMA_PATH"));
        CHECK(mentions(what, "<index_dir>/schemas/"));
    }

    TEST_CASE("the built-ins are declared with fields") {
        const auto& dft = ix::get_schema("dftracer");
        CHECK(dft.builtin);
        CHECK(dft.require == dstd::Vector<dstd::String>{"ph", "name", "ts"});
        CHECK(dft.roles.time == "ts");
        CHECK(dft.roles.duration == "dur");
        CHECK(dft.roles.entity == "pid");
        CHECK(dft.roles.phase == "ph");
        CHECK(dft.always_index.empty());
        const auto& generic = ix::get_schema("generic");
        CHECK(generic.fields.empty());
        CHECK(generic.require.empty());
    }

    TEST_CASE(
        "the genesis built-in is path-decoded with a stored runs row set") {
        const auto& g = ix::get_schema("genesis");
        CHECK(g.builtin);
        CHECK(g.decoder == ix::Decoder::Path);
        CHECK(g.require == dstd::Vector<dstd::String>{"gtype", "run"});
        CHECK(g.roles.time == "ts");
        CHECK(mentions(g.source, "data = where gtype != \"run\""));
        CHECK(mentions(g.source, "runs = where gtype == \"run\""));
        CHECK_FALSE(mentions(g.source, "files = "));

        dstd::Vector<dstd::StringView> lines = {
            R"({"gtype":"run","run":"ab","app":"laghos","nodes":1})"};
        for (int i = 0; i < 12; ++i)
            lines.push_back(
                R"({"gtype":"func","run":"ab","path":"main;f","ts":1,"count":2})");
        CHECK(ix::detect_schema(lines).id == "genesis");
        const dstd::Vector<dstd::StringView> counters = {
            R"({"gtype":"counter","run":"ab","path":"main","metric":"m"})"};
        CHECK(ix::detect_schema(counters).id == "genesis");
        const dstd::Vector<dstd::StringView> dft = {
            R"({"id":1,"name":"read","cat":"POSIX","ph":"X","ts":1})"};
        CHECK(ix::detect_schema(dft).id == "dftracer");
        const dstd::Vector<dstd::StringView> other = {
            R"({"kind":"x","id":"a"})"};
        CHECK(ix::detect_schema(other).id == "generic");
        const dstd::Vector<dstd::StringView> user = {
            R"({"type":"run","run":1})", R"({"type":"run","run":2})"};
        CHECK(ix::detect_schema(user).id == "generic");
    }

    TEST_CASE("a directory of specs loads once") {
        dftu_utils_test::TestEnvironment env(1);
        const auto dir = env.get_dir() + "/specs";
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        std::ofstream((dir + "/a.yaml").c_str()) << "id: dir_a\n";
        std::ofstream((dir + "/b.json").c_str())
            << R"({"id":"dir_b","extends":"dir_a"})";
        std::ofstream((dir + "/notes.txt").c_str()) << "not a spec";
        ix::load_schemas(dir);
        REQUIRE(ix::find_schema("dir_b") != nullptr);
        ix::load_schemas(dir);

        const auto bad = env.get_dir() + "/broken";
        REQUIRE(dstd::fs::create_directories(bad).has_value());
        std::ofstream((bad + "/c.yaml").c_str()) << "id: dir_c\nbogus: 1\n";
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
        const dstd::Vector<dstd::StringView> nginx = {
            R"({"status":200,"request_time":0.5})",
            R"({"status":404,"request_time":1.5,"x":1})"};
        const dstd::Vector<dstd::StringView> plain = {R"({"op":"read"})"};
        CHECK(ix::detect_schema(nginx).id == "det_nginx");
        CHECK(ix::detect_schema(plain).id == "generic");
    }

    TEST_CASE("explain reports each schema's share and the choice") {
        ix::register_schema(
            "id: exp_nginx\n"
            "fields: {status: {type: int}, request_time: {type: float}}\n",
            "exp.yaml");
        dstd::Vector<dstd::StringView> lines;
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
        REQUIRE(
            dstd::fs::create_directories(index_dir + "/schemas").has_value());
        // path_budget 1: the always-indexed field and the first other path.
        std::ofstream((index_dir + "/schemas/nginx.yaml").c_str())
            << "id: idx_nginx\n"
               "fields:\n"
               "  remote_addr: {type: string}\n"
               "  status: {type: int}\n"
               "  request_time: {type: float}\n"
               "  upstream: {type: string, optional: true, always_index: "
               "true}\n"
               "index: {path_budget: 1}\n";
        ix::IndexerOptions o;
        o.index_dir = index_dir;
        auto indexer =
            ix::Indexer::open({dstd::String(dstd::StringView(gz))}, o);
        indexer.build();
        const auto files = indexer.files();
        REQUIRE(files.size() == 1);
        CHECK(files[0].schema == "idx_nginx");

        const auto e = indexer.explain(R"(upstream == "u3")");
        REQUIRE(e.size() == 1);
        CHECK(e[0].read.size() < e[0].chunks);
        const dstd::String q = R"(upstream == "u3")";
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

TEST_SUITE("SchemaDetection") {
    static dstd::String dft_event(int i) {
        return R"({"id":)" + dstd::to_string(i) +
               R"(,"name":"read","cat":"POSIX","pid":1,"tid":2,"ts":)" +
               dstd::to_string(100 + i) +
               R"(,"dur":5,"ph":"X","args":{"fhash":"f1","ret":4096}})";
    }

    static dstd::String hash_line(const char* kind, int i) {
        return R"({"id":)" + dstd::to_string(i) + R"(,"name":")" + kind +
               R"(","cat":"dftracer","pid":1,"tid":2,"ph":"M","args":)"
               R"({"name":"/p/)" +
               dstd::to_string(i) + R"(","value":"h)" + dstd::to_string(i) +
               "\"}}";
    }

    static dstd::String genesis_record(int i) {
        return R"({"gtype":"func","run":"r1","path":"main;f)" +
               dstd::to_string(i) + R"(","name":"f","depth":1,"ts":)" +
               dstd::to_string(i) + R"(,"count":2,"dur":{"n":2}})";
    }

    const dstd::String RUN_LINE =
        R"({"gtype":"run","run":"r1","app":"laghos","nodes":1})";

    // `text` as `name` under a fresh directory, gzip when `gz`.
    static dstd::String write_file(dftu_utils_test::TestEnvironment & env,
                                   const dstd::String& name,
                                   const dstd::String& text, bool gz) {
        dstd::String plain(dstd::StringView(env.get_dir()));
        plain += "/";
        plain += name;
        std::ofstream(plain.c_str(), std::ios::binary) << text;
        if (!gz) return plain;
        const auto out = plain + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip(std::string(plain),
                                                       std::string(out)));
        (void)dstd::fs::remove(plain);
        return out;
    }

    static dstd::String chosen(const dstd::Vector<dstd::String>& lines) {
        dstd::Vector<dstd::StringView> views(lines.begin(), lines.end());
        return ix::detect_schema(views).id;
    }

    TEST_CASE("a corpus of JSON shapes is classified") {
        dstd::Vector<dstd::String> dft = {"["};
        for (int i = 0; i < 20; ++i) dft.push_back(dft_event(i));
        CHECK(chosen(dft) == "dftracer");

        dstd::Vector<dstd::String> meta_head;
        for (int i = 0; i < 300; ++i)
            meta_head.push_back(hash_line(i % 2 ? "FH" : "SH", i));
        for (int i = 0; i < 10; ++i) meta_head.push_back(dft_event(i));
        CHECK(chosen(meta_head) == "dftracer");

        dstd::Vector<dstd::String> run_first = {RUN_LINE};
        for (int i = 0; i < 5; ++i) run_first.push_back(genesis_record(i));
        CHECK(chosen(run_first) == "genesis");

        // Chrome trace events in the JSON array format: the dftracer decoder
        // reads them.
        dstd::Vector<dstd::String> chrome = {"["};
        for (int i = 0; i < 10; ++i)
            chrome.push_back(R"({"name":"MessageLoop","cat":"toplevel","ph":)"
                             R"("X","ts":)" +
                             dstd::to_string(i * 10) +
                             R"(,"dur":4,"pid":7,"tid":9,"args":{}},)");
        chrome.push_back("]");
        CHECK(chosen(chrome) == "dftracer");

        // `ph` and `name` without a timestamp are not a trace.
        dstd::Vector<dstd::String> ph_log;
        for (int i = 0; i < 10; ++i)
            ph_log.push_back(R"({"ph":"7.)" + dstd::to_string(i) +
                             R"(","name":"tank)" + dstd::to_string(i) +
                             R"(","site":"b"})");
        CHECK(chosen(ph_log) == "generic");

        dstd::Vector<dstd::String> web;
        for (int i = 0; i < 10; ++i)
            web.push_back(R"({"remote_addr":"10.0.0.1","time_local":"t)" +
                          dstd::to_string(i) +
                          R"(","request":"GET /a HTTP/1.1","status":200,)"
                          R"("body_bytes_sent":)" +
                          dstd::to_string(i * 100) + "}");
        CHECK(chosen(web) == "generic");

        dstd::Vector<dstd::String> otel;
        for (int i = 0; i < 10; ++i)
            otel.push_back(
                R"({"traceId":"a1","spanId":"s)" + dstd::to_string(i) +
                R"(","name":"GET /users","kind":2,"startTimeUnixNano":)" +
                dstd::to_string(1000 + i) + R"(,"endTimeUnixNano":)" +
                dstd::to_string(2000 + i) +
                R"(,"attributes":[{"key":"http.method","value":)"
                R"({"stringValue":"GET"}}],"status":{"code":0}})");
        CHECK(chosen(otel) == "generic");

        dstd::Vector<dstd::String> gh;
        for (int i = 0; i < 10; ++i)
            gh.push_back(R"({"id":")" + dstd::to_string(i) +
                         R"(","type":"PushEvent","actor":{"login":"u"},)"
                         R"("repo":{"name":"o/r"},"payload":{"size":1},)"
                         R"("public":true,"created_at":"2024-01-01T00:00:0)" +
                         dstd::to_string(i) + "Z\"}");
        CHECK(chosen(gh) == "generic");

        const auto none = ix::explain_schema({});
        CHECK(none.chosen->id == "generic");
        CHECK(none.objects == 0);
        CHECK(none.records == 0);
    }

    TEST_CASE("user schemas: paths, ties and optional fields") {
        // Paths resolve as JsonValue::at: an array index and a flat dotted
        // key.
        ix::register_schema(
            "id: det_otel\n"
            "fields:\n"
            "  trace_id: {type: string, path: traceId}\n"
            "  start: {type: int, path: startTimeUnixNano, role: time, "
            "unit: ns}\n"
            "  attr: {type: string, path: attributes.0.key}\n"
            "  method: {type: string, path: http.method}\n",
            "det_otel.yaml");
        dstd::Vector<dstd::String> otel;
        for (int i = 0; i < 10; ++i)
            otel.push_back(
                R"({"traceId":"a","startTimeUnixNano":)" + dstd::to_string(i) +
                R"(,"attributes":[{"key":"k"}],"http.method":"GET"})");
        CHECK(chosen(otel) == "det_otel");
        otel.push_back(R"({"traceId":"a","startTimeUnixNano":1})");
        otel.push_back(R"({"traceId":"a","startTimeUnixNano":1})");
        CHECK(chosen(otel) == "generic");

        // As specific as dftracer: the user schema wins.
        ix::register_schema(
            "id: det_tie\n"
            "fields: {ph: {type: string}, name: {type: string},\n"
            "  zz_tie: {type: int}}\n",
            "det_tie.yaml");
        dstd::Vector<dstd::String> tie;
        for (int i = 0; i < 5; ++i)
            tie.push_back(R"({"ph":"X","name":"n","ts":1,"zz_tie":)" +
                          dstd::to_string(i) + "}");
        CHECK(chosen(tie) == "det_tie");

        // Only optional fields: chosen by any declared path, never over a
        // schema with required paths.
        ix::register_schema(
            "id: det_gh\n"
            "fields:\n"
            "  kind: {type: string, path: gh_type, optional: true}\n"
            "  login: {type: string, path: gh_actor.login, optional: true}\n",
            "det_gh.yaml");
        dstd::Vector<dstd::String> gh;
        for (int i = 0; i < 10; ++i)
            gh.push_back(i % 2 ? R"({"gh_type":"PushEvent","id":1})"
                               : R"({"gh_actor":{"login":"u"},"id":2})");
        CHECK(chosen(gh) == "det_gh");
        dstd::Vector<dstd::String> dft;
        for (int i = 0; i < 10; ++i)
            dft.push_back(dft_event(i).insert(1, R"("gh_type":"x",)"));
        CHECK(chosen(dft) == "dftracer");
    }

    TEST_CASE("files: edges of the sample") {
        dftu_utils_test::TestEnvironment env(1);

        const auto empty = write_file(env, "empty.pfw", "", false);
        auto d = ix::explain_file_schema(empty);
        CHECK(d.chosen->id == "generic");
        CHECK(d.objects == 0);
        CHECK(d.records == 0);
        CHECK(
            ix::detect_file_schema(write_file(env, "bracket.pfw", "[\n", true))
                .id == "generic");

        // One line, no trailing newline, plain and gzip.
        CHECK(ix::detect_file_schema(
                  write_file(env, "one.pfw", dft_event(1), false))
                  .id == "dftracer");
        CHECK(ix::detect_file_schema(
                  write_file(env, "one_gz.pfw", dft_event(1), true))
                  .id == "dftracer");

        // A first record past the 16 MiB text cap is read whole.
        dstd::String big =
            R"({"id":0,"name":"read","cat":"POSIX","pid":1,)"
            R"("tid":2,"ts":1,"dur":5,"ph":"X","args":{"blob":")";
        big.append(17u * 1024 * 1024, 'x');
        big += "\"}}\n" + dft_event(2) + "\n";
        CHECK(
            ix::detect_file_schema(write_file(env, "big.pfw", big, true)).id ==
            "dftracer");

        // Genesis behind 1500 run lines, past the sample count.
        dstd::String gen;
        for (int i = 0; i < 1500; ++i)
            gen += R"({"gtype":"run","run":"r)" + dstd::to_string(i) + "\"}\n";
        for (int i = 0; i < 20; ++i) gen += genesis_record(i) + "\n";
        d = ix::explain_file_schema(write_file(env, "gen.pfw", gen, true));
        CHECK(d.chosen->id == "genesis");
        CHECK(d.records == 20);

        // Only metadata: dftracer, with no records to vote with.
        dstd::String meta;
        for (int i = 0; i < 10; ++i) meta += hash_line("HH", i) + "\n";
        d = ix::explain_file_schema(write_file(env, "meta.pfw", meta, true));
        CHECK(d.chosen->id == "dftracer");
        CHECK(d.records == 0);
    }

    TEST_CASE("file detection is cached until the file changes") {
        dftu_utils_test::TestEnvironment env(1);
        const auto path = env.get_dir() + "/c.ndjson";
        std::ofstream(path.c_str()) << R"({"k":1})" << "\n";
        CHECK(ix::detect_file_schema(path).id == "generic");
        std::ofstream(path.c_str()) << dft_event(1) << "\n"
                                    << dft_event(2) << "\n";
        CHECK(ix::detect_file_schema(path).id == "dftracer");
    }

    TEST_CASE("built-in sources equal their previous literals") {
        const dstd::String dft_old =
            R"(data = where ph not in ["M", 4];
files = where ph in ["M", 4] and name == "FH" | select fhash = args.value, path = args.name | distinct;
hosts = where ph in ["M", 4] and name == "HH" | select hhash = args.value, name = args.name | distinct;
strings = where ph in ["M", 4] and name == "SH" | select shash = args.value, value = args.name | distinct;
ranks = where ph in ["M", 4] and name == "PR" and args.name == "rank" | select pid, rank = args.value | distinct;
def args_fallback = true)";
        const dstd::String genesis_old =
            R"re(data = where gtype != "run";
runs = where gtype == "run" | select run, app, system, unique_input, nodes, ppn, papi_set, method, sketch_accuracy, leaf | distinct)re";
        for (const auto& [id, old] :
             {dstd::Pair<dstd::String, dstd::String>{"dftracer", dft_old},
              dstd::Pair<dstd::String, dstd::String>{"genesis", genesis_old}}) {
            const auto& s = ix::get_schema(id);
            CHECK(s.source == ix::merge_source({}, old, id));
            ix::RecordSchema before = s;
            before.source = ix::merge_source({}, old, id);
            CHECK(s.params_hash() == before.params_hash());
        }
    }
}
