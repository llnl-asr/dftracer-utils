// SPDX-License-Identifier: MIT
// The cases count walks, not time.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/hash/constants.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/store.h>
#include <dftracer/utils/trace/internal/parse_walks.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/parse_inflated.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/stat.h>
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
                                              const dstd::String& index_dir) {
    Runtime rt(4);
    index::build::IndexBuildBatchResult result;
    auto task = coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
        auto config = dstd::make_shared<index::build::IndexBuildBatchConfig>();
        config->file_paths = {dstd::String(dstd::StringView(file))};
        config->index_dir = index_dir;
        config->parallelism = 4;
        result =
            co_await index::build::BatchBuilder::process(std::move(config));
    });
    rt.submit(std::move(task), "parse-walks").wait();
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
    {"dftu.idx.bloom", 397815554527540866ULL},
    {"dftu.idx.core.catalog", 13277922137834801935ULL},
    {"dftu.idx.core.members", 12223201460428748558ULL},
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
        const dstd::String file = write_trace(env.get_dir());
        std::uint64_t with_args = 0;
        for (int i = 0; i < RECORDS; ++i)
            if (has_args_object(shape_of(i))) ++with_args;

        trace::internal::parse_walks::g_record_walks = 0;
        trace::internal::parse_walks::g_args_walks = 0;
        trace::internal::parse_walks::g_count = true;
        build_one(file, env.get_dir() + "/idx");
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
        const dstd::String file = write_trace(env.get_dir());
        build_one(file, env.get_dir() + "/idx");
        const dstd::String path =
            trace::internal::determine_index_path(file, env.get_dir() + "/idx");
        const auto digests = namespace_digests(
            index::store::internal::normalize_index_root(path), env.get_dir());
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
            "{\"id\":3,\"name\":\"ctl\tchar\",\"cat\":\"c\",\"pid\":1,"
            "\"tid\":1,\"ph\":\"X\",\"ts\":7}",
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
            dftu_utils_test::write_gz_trace(env.get_dir() + "/bad.pfw.gz", out);
        const auto result = build_one(file, env.get_dir() + "/idx");
        REQUIRE(result.results.size() == 1);
        CHECK(result.results[0].events_processed == std::size_t{GOOD});
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
        const dstd::String file = dftu_utils_test::write_gz_trace(
            env.get_dir() + "/long.pfw.gz", out);
        trace::internal::parse_walks::g_record_walks = 0;
        trace::internal::parse_walks::g_count = true;
        const auto result = build_one(file, env.get_dir() + "/idx");
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
