// SPDX-License-Identifier: MIT
// The cases count walks, not time.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/hash/constants.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/store.h>
#include <dftracer/utils/trace/internal/parse_walks.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/parse_inflated.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <generator_ok.h>
#include <sys/stat.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <cstdint>
#include <ctime>
#include <iterator>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <variant>

using namespace dftracer::utils;

namespace {

constexpr int RECORDS = 4000;
constexpr int SHAPES = 8;
constexpr std::time_t FIXED_MTIME = 1700000000;

// Schema detection samples the head of the file, so records that do not look
// like dftracer events come after it.
constexpr int HEAD_RECORDS = 1000;

int shape_of(int i) { return i < HEAD_RECORDS ? i % 3 : i % SHAPES; }

// Shape 7 has no phase, so it is walked once and then dropped.
bool has_args_object(int shape) {
    return shape == 0 || shape == 2 || shape == 3 || shape == 5 || shape == 6;
}

dstd::String write_trace(const dstd::String& dir) {
    dstd::String out;
    out += "[\n";
    for (int i = 0; i < RECORDS; ++i) {
        const int pid = 1 + i % 4;
        const int ts = 1000 + i * 10;
        out += common::str_cat(R"({"id":)", i);
        switch (shape_of(i)) {
            case 0:
                out += common::str_cat(
                    R"(,"name":"read","cat":"POSIX","pid":)", pid,
                    R"(,"tid":1,"ph":"X","ts":)", ts,
                    R"(,"dur":3,"args":{"fname":"/data/f)", i % 37,
                    R"(","size":)", i % 4096,
                    R"(,"fhash":"f00d","hhash":"a1b2c3d4e5f60718","level":)",
                    i % 3, "}}\n");
                break;
            case 1:
                out += common::str_cat(
                    R"(,"name":"compute","cat":"APP","pid":)", pid,
                    R"(,"tid":2,"ph":"X","ts":)", ts, R"(,"dur":7})", "\n");
                break;
            case 2:
                out += common::str_cat(
                    R"(,"name":"write","cat":"POSIX","pid":)", pid,
                    R"(,"tid":1,"ph":"X","ts":)", ts,
                    R"(,"dur":5,"args":{"io":{"off":)", i,
                    R"(,"len":512},"hosts":["n1","n2"],"note":null,)",
                    R"("ratio":0.5}})", "\n");
                break;
            case 3:
                out += common::str_cat(R"(,"name":"open","cat":"POSIX","pid":)",
                                       pid, R"(,"tid":1,"ph":"X","ts":)", ts,
                                       R"(,"dur":1,"args":{}})", "\n");
                break;
            case 4:
                out +=
                    common::str_cat(R"(,"name":"close","cat":"POSIX","pid":)",
                                    pid, R"(,"tid":1,"ph":"X","ts":)", ts,
                                    R"(,"dur":1,"args":7})", "\n");
                break;
            case 5:
                out += common::str_cat(
                    R"(,"name":"stat","cat":"POSIX","pid":)", pid,
                    R"(,"tid":3,"ph":"X","ts":)", ts,
                    R"(,"dur":2,"extra":"tag)", i % 5,
                    R"(","meta":{"k":[1,2],"z":null},"args":{"fname":"/d/s)",
                    i % 11, R"(","ret":-)", i, "}}\n");
                break;
            case 6:
                out += common::str_cat(
                    R"(,"name":"seek","cat":"STDIO","pid":"7","tid":1,)",
                    R"("ph":"X","ts":)", ts,
                    R"(,"dur":2.5,"args":{"neg":-3,"big":)",
                    "18446744073709551615",
                    R"(,"flag":true,"off":false,"ratio":1.25,)",
                    R"("tag":"x"}})", "\n");
                break;
            default:
                out +=
                    common::str_cat(R"(,"name":"nophase","cat":"POSIX","pid":)",
                                    pid, R"(,"ts":)", ts, "}\n");
        }
    }
    out += "]\n";
    const dstd::String path =
        dftu_utils_test::write_stored_gz_trace(dir + "/trace.pfw.gz", out);
    const timespec fixed[2] = {{FIXED_MTIME, 0}, {FIXED_MTIME, 0}};
    REQUIRE(::utimensat(AT_FDCWD, path.c_str(), fixed, 0) == 0);
    return path;
}

index::build::IndexBuildBatchResult build_one(const dstd::String& file,
                                              const dstd::String& index_dir,
                                              std::size_t checkpoint_size = 0) {
    Runtime rt(4);
    index::build::IndexBuildBatchResult result;
    auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
        auto config = dstd::make_shared<index::build::IndexBuildBatchConfig>();
        config->file_paths = {dstd::String(dstd::StringView(file))};
        config->index_dir = index_dir;
        config->parallelism = 4;
        if (checkpoint_size > 0) config->checkpoint_size = checkpoint_size;
        result =
            co_await index::build::BatchBuilder::process(std::move(config));
    });
    dftu_utils_test::ok(rt.submit(std::move(task), "parse-walks").wait());
    rt.shutdown();
    REQUIRE(result.indexed == 1);
    return result;
}

// A namespace missing here fails the check.
struct NamespaceDigest {
    dstd::StringView name;
    std::uint64_t digest;
};
constexpr NamespaceDigest RECORDED[] = {
    {"dftu.idx.bloom", 7883963652881105830ULL},
    {"dftu.idx.core.catalog", 13277922137834801935ULL},
    {"dftu.idx.core.members", 7697722678485111383ULL},
    {"dftu.idx.core.profile", 12883975498841085017ULL},
    {"dftu.idx.core.rowset", 6585397510590508660ULL},
    {"dftu.idx.counts", 14528743384093093223ULL},
    {"dftu.idx.dft.metadata", 8564637570051539016ULL},
    {"dftu.idx.dft.stats", 13276905867863997891ULL},
    {"dftu.idx.postings", 10579150546114072638ULL},
    {"dftu.idx.zonemap", 14397360174130556973ULL},
};

// The test directory is cut out of each record, since its length varies. A
// record is its namespace, scope paths, key and value.
dstd::Map<dstd::String, std::uint64_t> namespace_digests(
    dstd::StringView index_path, dstd::StringView dir) {
    dstd::Map<dstd::String, std::uint64_t> out;
    const auto without_dir = [&dir](dstd::StringView bytes) {
        dstd::String text(bytes);
        for (auto at = text.find(dir); at != dstd::String::npos;
             at = text.find(dir, at))
            text.erase(at, dir.size());
        return text;
    };
    auto opened = dftracer::utils::store::Store::open(dstd::String(index_path) +
                                                      "/store");
    REQUIRE(opened.has_value());
    auto snapshot = opened->snapshot();
    REQUIRE(snapshot.has_value());
    auto declared = snapshot->namespaces();
    REQUIRE(declared.has_value());
    for (const auto& decl : *declared) {
        dftracer::utils::store::Namespace ns;
        ns.spec.name = decl.name;
        ns.spec.format_version = decl.format_version;
        auto [it, fresh] =
            out.emplace(dstd::String(decl.name.data(), decl.name.size()),
                        dstd::hash::FNV1A_OFFSET_BASIS_LEGACY);
        std::uint64_t& h = it->second;
        const auto mix = [&h](dstd::StringView bytes) {
            for (unsigned char c : bytes) h = (h ^ c) * dstd::hash::FNV1A_PRIME;
            h = (h ^ 0xFF) * dstd::hash::FNV1A_PRIME;
        };
        auto cursor = snapshot->scan_all(ns, {}, {});
        for (; !cursor.at_end(); cursor.next()) {
            dstd::String scope;
            for (const auto& path : cursor.scope().paths) {
                scope.append(path.data(), path.size());
                scope.push_back('\0');
            }
            const auto key = cursor.key();
            auto value = cursor.value();
            REQUIRE(value.has_value());
            mix(without_dir(scope));
            mix(without_dir(dstd::StringView(key.as<char>(), key.size())));
            mix(without_dir(
                dstd::StringView(value->as<char>(), value->size())));
        }
        REQUIRE_FALSE(cursor.error().has_value());
    }
    return out;
}

coro::CoroTask<std::size_t> count_json(
    coro::AsyncGenerator<utilities::reader::JsonLine> gen) {
    std::size_t n = 0;
    while (auto line = co_await gen.next()) ++n;
    co_return n;
}

// Resolves interned ids, so events from different interns compare.
dstd::String describe(const trace::views::detail::FoldEvent& ev,
                      const dstd::StringIntern& in) {
    const auto text = [&](std::uint32_t id) {
        return id == dstd::StringIntern::NO_ID ? dstd::String("<none>")
                                               : dstd::String(in.resolve(id));
    };
    dstd::String o;
    o += common::str_cat(
        "cat=", text(ev.cat_id), " name=", text(ev.name_id),
        " fhash=", text(ev.fhash_id), " hhash=", text(ev.hhash_id),
        " pid=", ev.pid, " tid=", ev.tid, " ts=", ev.ts, " dur=", ev.dur,
        " phase=", static_cast<int>(ev.phase), " has_dur=", ev.has_dur,
        " by_path=", ev.by_path, " unconverted=", ev.unconverted, "\n");
    const auto list = [&](const char* label, const auto& pairs) {
        o += common::str_cat(label, ":");
        for (const auto& [k, v] : pairs) {
            o += common::str_cat(" ", text(k), "=");
            std::visit(
                [&](const auto& x) {
                    using T = std::decay_t<decltype(x)>;
                    if constexpr (std::is_same_v<T, std::uint32_t>)
                        o += common::str_cat("s:", text(x));
                    else
                        o += common::str_cat(typeid(T).name(), ":", x);
                },
                v);
        }
        o += "\n";
    };
    list("args", ev.args);
    list("top", ev.top_fields);
    o += "specials:";
    for (const auto& sp : ev.specials)
        o += common::str_cat(" ", text(sp.key), "/", static_cast<int>(sp.kind),
                             "/", sp.top);
    o += "\nleaves:";
    for (const auto& [id, tag] : ev.schema_leaves)
        o += common::str_cat(" ", text(id), "#", static_cast<int>(tag));
    return o;
}

}  // namespace

TEST_SUITE("IndexParseWalks") {
    TEST_CASE("each record and each args object is walked once") {
        dftu_utils_test::TestEnvironment env(10);
        const dstd::String file = write_trace(env.dir());
        std::uint64_t with_args = 0;
        for (int i = 0; i < RECORDS; ++i)
            if (has_args_object(shape_of(i))) ++with_args;

        trace::internal::parse_walks::g_record_walks = 0;
        trace::internal::parse_walks::g_args_walks = 0;
        trace::internal::parse_walks::g_count = true;
        build_one(file, env.dir() + "/idx");
        trace::internal::parse_walks::g_count = false;

        MESSAGE("record walks "
                << trace::internal::parse_walks::g_record_walks.load()
                << " for " << RECORDS << " records, args walks "
                << trace::internal::parse_walks::g_args_walks.load() << " for "
                << with_args << " args objects");
        CHECK(trace::internal::parse_walks::g_record_walks.load() ==
              std::uint64_t{RECORDS});
        CHECK(trace::internal::parse_walks::g_args_walks.load() == with_args);
    }

    TEST_CASE("the index of a fixed trace is unchanged") {
        dftu_utils_test::TestEnvironment env(10);
        const dstd::String file = write_trace(env.dir());
        build_one(file, env.dir() + "/idx");
        const dstd::String path =
            trace::internal::determine_index_path(file, env.dir() + "/idx");
        const auto digests =
            namespace_digests(index::normalize_index_root(path), env.dir());
        for (const auto& [name, digest] : digests)
            MESSAGE("namespace " << name << " " << digest);
        CHECK(digests.size() == std::size(RECORDED));
        for (const auto& want : RECORDED) {
            const auto it = digests.find(dstd::String(want.name));
            REQUIRE(it != digests.end());
            CHECK(it->second == want.digest);
        }
    }

    TEST_CASE("a malformed record drops only itself") {
        constexpr int GOOD = 3000;
        const dstd::String bad[] = {
            R"({"id":1,"name":"cut","cat":"c","pid":1,"tid":1,"ph":"X","ts":5,"args":{"a":)",
            R"({"id":2,"name":"quote","cat":"c","pid":1,"tid":1,"ph":"X","ts":6,"args":{"s":"a"b"}})",
            "not json at all",
            "[1,2,3]",
        };
        dstd::String out;
        out += "[\n";
        for (int i = 0; i < GOOD; ++i) {
            out += common::str_cat(
                R"({"id":)", i,
                R"(,"name":"read","cat":"POSIX","pid":1,"tid":1,"ph":"X",)",
                R"("ts":)", 1000 + i, R"(,"dur":2,"args":{"size":)", i,
                R"(,"huge":123456789012345678901234567890,"neg":-5}})", "\n");
            if (i % 600 == 0)
                for (const auto& line : bad) out += common::str_cat(line, "\n");
        }
        out += "]\n";
        dftu_utils_test::TestEnvironment env(10);
        const dstd::String file =
            dftu_utils_test::write_gz_trace(env.dir() + "/bad.pfw.gz", out);
        const auto result = build_one(file, env.dir() + "/idx");
        REQUIRE(result.results.size() == 1);
        CHECK(result.results[0].events_processed == std::size_t{GOOD});
    }

    TEST_CASE("a line the DOM parser refuses is indexed and found") {
        struct Odd {
            const char* name;
            dstd::String fields;
            const char* needle;
            const char* equals;
        };
        dstd::String deep;
        for (int i = 0; i < 3000; ++i) deep += R"({"a":)";
        deep += "1";
        for (int i = 0; i < 3000; ++i) deep += "}";
        const Odd odds[] = {
            {"9 MiB string",
             common::str_cat(R"("s":")", dstd::String(9 * 1024 * 1024, 'x'),
                             R"(","tag":"n8")"),
             "n8", nullptr},
            {"1e400", R"("big":1e400,"tag":"n1")", "n1", nullptr},
            {"invalid escape", R"("s":"a\qb","tag":"n2")", "n2", nullptr},
            {"20 digits", R"("n":18446744073709551616,"tag":"n3")", "n3",
             nullptr},
            {"bad UTF-8", dstd::String("\"s\":\"a\xffz\",\"tag\":\"n4\""), "n4",
             "a?z"},
            {"control byte", dstd::String("\"s\":\"a\tz\",\"tag\":\"n5\""),
             "n5", "a?z"},
            {"deep nesting",
             common::str_cat(R"("deep":)", deep, R"(,"tag":"n6")"), "n6",
             nullptr},
            {"invalid escape in a key", R"("k\q":1,"tag":"n7")", "n7", nullptr},
        };
        constexpr int GOOD = 2000;
        for (const bool dftracer : {true, false}) {
            for (const Odd& odd : odds) {
                INFO(dstd::String(odd.name));
                INFO(dstd::String(dftracer ? "dftracer lines"
                                           : "generic lines"));
                const dstd::String where = dftracer ? "args." : "";
                dstd::String out;
                for (int i = 0; i < GOOD; ++i) {
                    out += dftracer
                               ? common::str_cat(
                                     R"({"id":)", i,
                                     R"(,"name":"read","cat":"POSIX","pid":1,)",
                                     R"("tid":1,"ph":"X","ts":)", 1000 + i,
                                     R"(,"dur":2,"args":{"size":)", i,
                                     R"(,"tag":"plain"}})", "\n")
                               : common::str_cat(
                                     R"({"op":"read","host":"h1","size":)", i,
                                     R"(,"tag":"plain"})", "\n");
                    if (i == GOOD / 2)
                        out +=
                            dftracer
                                ? common::str_cat(
                                      R"({"id":9,"name":"odd","cat":"c",)",
                                      R"("pid":1,"tid":1,"ph":"X","ts":5,)",
                                      R"("dur":2,"args":{)", odd.fields, "}}\n")
                                : common::str_cat(R"({"op":"odd",)", odd.fields,
                                                  "}\n");
                }
                dftu_utils_test::TestEnvironment env(10);
                const dstd::String file = dftu_utils_test::write_gz_trace(
                    env.dir() + "/odd.pfw.gz", out);
                const auto result =
                    build_one(file, env.dir() + "/idx", 32 * 1024);
                REQUIRE(result.results.size() == 1);
                CHECK(result.results[0].events_processed ==
                      std::size_t{GOOD + 1});

                utilities::reader::TraceReader reader(
                    {.file_path = file, .index_dir = env.dir() + "/idx"});
                REQUIRE(reader.has_index());
                const auto found = [&](const dstd::String& query) {
                    utilities::reader::ReadConfig rc;
                    rc.query = query;
                    return dftu_utils_test::ok(
                        count_json(
                            dftu_utils_test::ok_items(reader.read_json(rc)))
                            .get());
                };
                CHECK(found(common::str_cat(where, "tag == \"", odd.needle,
                                            '"')) == 1);
                if (odd.equals)
                    CHECK(found(common::str_cat(where, "s == \"", odd.equals,
                                                '"')) == 1);
            }
        }
    }

    TEST_CASE("a refused line keeps its place among the lines around it") {
        const auto run = [](const dstd::Vector<dstd::String>& lines,
                            std::size_t& dom_parses) {
            dstd::String text;
            for (const auto& line : lines) text += common::str_cat(line, "\n");
            text += dstd::String(simdjson::SIMDJSON_PADDING, '\0');
            simdjson::dom::parser parser;
            dstd::Vector<dstd::String> seen;
            dom_parses = 0;
            trace::parse_lines(
                parser, text.data(), text.size() - simdjson::SIMDJSON_PADDING,
                [&](simdjson::dom::element, dstd::StringView src) {
                    ++dom_parses;
                    seen.push_back(common::str_cat("D:", src));
                },
                [&](dstd::StringView line, std::size_t capacity) {
                    CHECK(capacity >= line.size() + simdjson::SIMDJSON_PADDING);
                    seen.push_back(common::str_cat("R:", line));
                });
            return seen;
        };
        const auto check = [](const dstd::Vector<dstd::String>& seen,
                              const dstd::Vector<dstd::String>& want) {
            REQUIRE(seen.size() == want.size());
            for (std::size_t i = 0; i < seen.size(); ++i)
                CHECK(seen[i] == want[i]);
        };
        const dstd::String g1 = R"({"ph":"X","name":"g1","ts":1})";
        const dstd::String o1 =
            R"({"ph":"X","name":"o1","ts":2,"args":{"big":1e400}})";
        const dstd::String g2 = R"({"ph":"X","name":"g2","ts":3})";
        const dstd::String o2 =
            R"({"ph":"X","name":"o2","ts":4,"args":{"s":"a\q"}})";
        const dstd::String g3 = R"({"ph":"X","name":"g3","ts":6})";
        const dstd::String g4 = R"({"ph":"X","name":"g4","ts":7})";
        const dstd::String g5 = R"({"ph":"X","name":"g5","ts":8})";
        const dstd::String o3 =
            R"({"ph":"X","name":"o3","ts":9,"args":{"n":18446744073709551616}})";
        const dstd::String g6 = R"({"ph":"X","name":"g6","ts":10})";
        const dstd::String cut =
            R"({"ph":"X","name":"cut","ts":5,"args":{"a":)";

        std::size_t dom_parses = 0;
        check(run({g1, o1, g2, o2, g3, o3, g6}, dom_parses),
              {"D:" + g1, "R:" + o1, "D:" + g2, "R:" + o2, "D:" + g3, "R:" + o3,
               "D:" + g6});
        CHECK(dom_parses == 4);

        check(run({g1, common::str_cat(g4, g5), g6}, dom_parses),
              {"D:" + g1, "D:" + g4, "D:" + g5, "D:" + g6});
        CHECK(dom_parses == 4);

        check(run({common::str_cat(g1, " ", o1), g2}, dom_parses),
              {"D:" + g1, "R:" + o1, "D:" + g2});
        CHECK(dom_parses == 2);

        check(run({common::str_cat(g1, ", ", o1), g2}, dom_parses),
              {"D:" + g1, "R:" + o1, "D:" + g2});

        const dstd::String cut_nested =
            R"({"ph":"X","name":"cut","ts":5,"a":1e400,"b":{"c":1})";
        check(run({g1, cut_nested, g2}, dom_parses),
              {"D:" + g1, "R:" + cut_nested, "D:" + g2});

        check(run({g1, o1, g2, cut, g3, o2, g4}, dom_parses),
              {"D:" + g1, "R:" + o1, "D:" + g2, "R:" + cut, "D:" + g3,
               "R:" + o2, "D:" + g4});
        CHECK(dom_parses == 4);
    }

    TEST_CASE("refused lines among good ones are each indexed once and found") {
        constexpr int ROUNDS = 6;
        const dstd::String odd[] = {
            R"("big":1e400)",
            R"("s":"a\qb")",
            R"("n":18446744073709551616)",
        };
        dstd::String out;
        out += "[\n";
        dstd::Vector<dstd::String> tags;
        int id = 0;
        for (int r = 0; r < ROUNDS; ++r) {
            for (int k = 0; k < 3; ++k) {
                const dstd::String tag = common::str_cat("g", id);
                out += common::str_cat(
                    R"({"id":)", id,
                    R"(,"name":"read","cat":"POSIX","pid":1,"tid":1,"ph":"X",)",
                    R"("ts":)", 1000 + id, R"(,"dur":2,"args":{"tag":")", tag,
                    R"("}})", "\n");
                tags.push_back(tag);
                ++id;
            }
            const dstd::String tag = common::str_cat("o", id);
            out += common::str_cat(
                R"({"id":)", id,
                R"(,"name":"read","cat":"POSIX","pid":1,"tid":1,"ph":"X",)",
                R"("ts":)", 1000 + id, R"(,"dur":2,"args":{"tag":")", tag,
                R"(",)", odd[r % 3], "}}\n");
            tags.push_back(tag);
            ++id;
        }
        out += "]\n";
        dftu_utils_test::TestEnvironment env(10);
        const dstd::String file =
            dftu_utils_test::write_gz_trace(env.dir() + "/mixed.pfw.gz", out);
        const auto result = build_one(file, env.dir() + "/idx");
        REQUIRE(result.results.size() == 1);
        CHECK(result.results[0].events_processed == tags.size());

        utilities::reader::TraceReader reader(
            {.file_path = file, .index_dir = env.dir() + "/idx"});
        REQUIRE(reader.has_index());
        for (const auto& tag : tags) {
            utilities::reader::ReadConfig rc;
            rc.query = common::str_cat("args.tag == \"", tag, '"');
            INFO(tag);
            CHECK(dftu_utils_test::ok(count_json(dftu_utils_test::ok_items(
                                                     reader.read_json(rc)))
                                          .get()) == 1);
        }
    }

    TEST_CASE("two records on one line and a record over two lines are kept") {
        constexpr int GOOD = 40;
        dstd::String out;
        out += "[\n";
        const auto record = [](const char* tag, int id) {
            return common::str_cat(
                R"({"id":)", id,
                R"(,"name":"read","cat":"POSIX","pid":1,"tid":1,"ph":"X",)",
                R"("ts":)", 1000 + id, R"(,"dur":2,"args":{"tag":")", tag,
                R"("}})");
        };
        for (int i = 0; i < GOOD; ++i) {
            out += common::str_cat(record(common::str_cat("g", i).c_str(), i),
                                   "\n");
            if (i == 10)
                out +=
                    common::str_cat(record("a1", 100), record("a2", 101), "\n");
            if (i == 20) {
                const dstd::String whole = record("s1", 102);
                const std::size_t cut = whole.find(R"("pid")");
                out += common::str_cat(whole.substr(0, cut), "\n",
                                       whole.substr(cut), "\n");
            }
        }
        out += "]\n";
        dftu_utils_test::TestEnvironment env(10);
        const dstd::String file =
            dftu_utils_test::write_gz_trace(env.dir() + "/shape.pfw.gz", out);
        const auto result = build_one(file, env.dir() + "/idx");
        REQUIRE(result.results.size() == 1);
        CHECK(result.results[0].events_processed == std::size_t{GOOD + 3});

        utilities::reader::TraceReader reader(
            {.file_path = file, .index_dir = env.dir() + "/idx"});
        REQUIRE(reader.has_index());
        for (const char* tag : {"g0", "g39"}) {
            utilities::reader::ReadConfig rc;
            rc.query = common::str_cat("args.tag == \"", tag, '"');
            INFO(tag);
            CHECK(dftu_utils_test::ok(count_json(dftu_utils_test::ok_items(
                                                     reader.read_json(rc)))
                                          .get()) == 1);
        }
    }

    TEST_CASE("a refused record after another on one line is indexed") {
        constexpr int GOOD = 40;
        const auto record = [](int id, const char* extra) {
            return common::str_cat(
                R"({"id":)", id,
                R"(,"name":"read","cat":"POSIX","pid":1,"tid":1,"ph":"X",)",
                R"("ts":)", 1000 + id, R"(,"dur":2,"args":{)", extra, "}}");
        };
        dstd::String out;
        out += "[\n";
        for (int i = 0; i < GOOD; ++i) {
            out += common::str_cat(record(i, R"("a":1)"), "\n");
            if (i == 10)
                out += common::str_cat(record(100, R"("a":1)"), " ",
                                       record(101, R"("a":1e400)"), "\n");
        }
        out += "]\n";
        dftu_utils_test::TestEnvironment env(10);
        const dstd::String file =
            dftu_utils_test::write_gz_trace(env.dir() + "/pair.pfw.gz", out);
        const auto result = build_one(file, env.dir() + "/idx");
        REQUIRE(result.results.size() == 1);
        CHECK(result.results[0].events_processed == std::size_t{GOOD + 2});
        CHECK(result.results[0].dropped_lines == 0);
    }

    TEST_CASE("a line that cannot be read in full is counted and dropped") {
        constexpr int GOOD = 40;
        dstd::String out;
        out += "[\n";
        for (int i = 0; i < GOOD; ++i) {
            out += common::str_cat(
                R"({"id":)", i,
                R"(,"name":"read","cat":"POSIX","pid":1,"tid":1,"ph":"X",)",
                R"("ts":)", 1000 + i, R"(,"dur":2,"args":{"a":1}})", "\n");
            if (i == 10)
                out += R"({"id":99,"name":"cut","ph":"X","a":1e400,"b":{"c":1})"
                       "\n";
        }
        out += "]\n";
        dftu_utils_test::TestEnvironment env(10);
        const dstd::String file =
            dftu_utils_test::write_gz_trace(env.dir() + "/cut.pfw.gz", out);
        const auto result = build_one(file, env.dir() + "/idx");
        REQUIRE(result.results.size() == 1);
        CHECK(result.results[0].events_processed == std::size_t{GOOD});
        CHECK(result.results[0].dropped_lines == 1);
    }

    TEST_CASE("a record longer than a parse batch is indexed") {
        constexpr int LINES = 40;
        constexpr std::size_t LONG_BYTES = 3 * trace::PARSE_BATCH_BYTES;
        dstd::String out;
        out += "[\n";
        for (int i = 0; i < LINES; ++i) {
            out += common::str_cat(
                R"({"id":)", i,
                R"(,"name":"read","cat":"POSIX","pid":1,"tid":1,"ph":"X",)",
                R"("ts":)", 1000 + i, R"(,"dur":2,"args":{"size":)", i);
            if (i == 20)
                out += common::str_cat(R"(,"note":")",
                                       dstd::String(LONG_BYTES, 'x'), '"');
            out += "}}\n";
        }
        out += "]\n";
        dftu_utils_test::TestEnvironment env(10);
        const dstd::String file =
            dftu_utils_test::write_gz_trace(env.dir() + "/long.pfw.gz", out);
        trace::internal::parse_walks::g_record_walks = 0;
        trace::internal::parse_walks::g_count = true;
        const auto result = build_one(file, env.dir() + "/idx");
        trace::internal::parse_walks::g_count = false;
        REQUIRE(result.results.size() == 1);
        CHECK(result.results[0].events_processed == std::size_t{LINES});
        CHECK(trace::internal::parse_walks::g_record_walks.load() ==
              std::uint64_t{LINES});
    }

    TEST_CASE("the one-walk decode equals the old three-walk decode") {
        const dstd::String records[] = {
            R"({"ph":"X","name":"n","cat":"c","pid":1,"tid":2,"ts":3,"dur":4,"args":{"a":1,"b":"x","fhash":"f","hhash":"h"}})",
            R"({"ph":"X","name":"n","args":{"a":1},"cat":"c"})",
            R"({"args":{"a":1},"ph":"X","name":"n","cat":"c","pid":5})",
            R"({"ph":"X","args":{"a":1},"args":{"b":2}})",
            R"({"ph":"X","args":{"a":1},"args":5})",
            R"({"ph":"X","args":5,"args":{"a":1}})",
            R"({"ph":"X","args":{}})",
            R"({"ph":"X","args":{},"x":1})",
            R"({"ph":"X","args":7,"x":[1,{"y":null}]})",
            R"({"ph":"X","args":[1,2,{"z":true}]})",
            R"({"ph":"X","args":"s","name":"n"})",
            R"({"ph":"X","args":{"io":{"off":1,"len":[1,2,{"k":"v"}]},"e":{},"f":[],"n":null,"t":true,"d":0.5,"neg":-3}})",
            R"({"ph":"X","name":"n","name":"m","ts":1,"ts":2,"pid":1,"pid":-1})",
            R"({"name":"n","cat":"c","ts":1,"args":{"a":1}})",
            R"({"ph":1,"ts":-5,"pid":2.5,"tid":"7","dur":1.5})",
            R"({"ph":"X","ts":18446744073709551615,"pid":-1,"dur":9223372036854775808})",
            R"({"ph":"X","cat":5,"name":false,"type":3,"id":9})",
            R"({"ph":"X","args":{"big":18446744073709551615,"huge":123456789012345678901234567890,"i":-9223372036854775808}})",
            R"({"ph":"X","args":{"fhash":5,"hhash":{"a":1}}})",
            R"({"ph":"X","args":{"fhash":"f","k":"v"},"fhash":"top","": 1})",
            R"({"ph":"X","":{"a":1},"a":{"":2}})",
            R"({"ph":null,"name":"n"})",
            R"([1,2])",
        };
        std::size_t compared = 0;
        for (const auto& text : records)
            for (const bool needs_args : {false, true})
                for (const bool capture : {false, true}) {
                    simdjson::dom::parser parser;
                    parser.number_as_string(true);
                    simdjson::dom::element root;
                    const dstd::String padded =
                        text + dstd::String(simdjson::SIMDJSON_PADDING, ' ');
                    if (parser.parse(padded.data(), text.size(), false)
                            .get(root))
                        continue;
                    const dstd::String label =
                        text + " needs_args=" + (needs_args ? "1" : "0") +
                        " capture=" + (capture ? "1" : "0");

                    dstd::StringIntern old_intern;
                    trace::views::detail::DecodeHints old_hints;
                    trace::views::detail::FoldEvent old_ev;
                    trace::DFTracerEvent probe;
                    simdjson::dom::element args;
                    bool has_args = false;
                    const bool kept = trace::DFTracerEvent::parse_scalars(
                        root, probe, args, has_args);
                    trace::views::detail::extract_fold_event_into(
                        old_ev, root, old_intern, needs_args, nullptr, capture,
                        &old_hints);

                    dstd::StringIntern new_intern;
                    trace::views::detail::DecodeHints new_hints;
                    trace::views::detail::FoldEventBuffer batch;
                    const bool appended =
                        trace::views::detail::append_dftracer_record(
                            batch, root, new_intern, needs_args, capture,
                            new_hints, nullptr);
                    INFO(label);
                    REQUIRE(appended == kept);
                    if (!kept) {
                        CHECK(batch.empty());
                        continue;
                    }
                    REQUIRE(batch.size() == 1);
                    CHECK(describe(batch.back(), new_intern) ==
                          describe(old_ev, old_intern));
                    ++compared;
                }
        CHECK(compared > 60);
    }
}
