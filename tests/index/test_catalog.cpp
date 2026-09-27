#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace dftracer::utils;
namespace store = dftracer::utils::index::store;
using store::ColumnType;
using store::IndexExtension;
using store::PathStat;
using store::PathType;

namespace {

std::string event(int i, const std::string& name, const std::string& args) {
    std::ostringstream o;
    o << R"({"id":)" << i << R"(,"name":")" << name
      << R"(","cat":"POSIX","pid":1,"tid":2,"ph":"X","ts":)" << 1000 + i
      << R"(,"dur":5,"type":"x","args":)" << args << "}\n";
    return o.str();
}

// 200 events: nested, array, bool, big uint, double and null args; `mix` is a
// number on the first 100 events and a string after; only the last carries
// `retry`. A metadata record carries args the catalog must not list.
std::string write_fixture(const std::string& dir) {
    std::string body = "[\n";
    body +=
        R"({"name":"FH","ph":"M","pid":1,"tid":1,"args":{"name":"/a","value":"abc"}})"
        "\n";
    for (int i = 0; i < 200; ++i) {
        std::ostringstream a;
        a << R"({"size":)" << i * 10 << R"(,"ratio":)" << i << ".5"
          << R"(,"flag":)" << (i % 2 ? "true" : "false")
          << R"(,"big":18446744073709551000,"io":{"off":)" << i
          << R"(,"mode":"r"},"hosts":["h1","h2"],"nul":null,"mix":)";
        if (i < 100)
            a << i;
        else
            a << R"("s")";
        if (i == 199) a << R"(,"retry":1)";
        a << "}";
        body += event(i, i % 3 ? "read" : "write", a.str());
    }
    body += "]\n";
    return dftu_utils_test::write_gz_trace(dir + "/fixture.pfw.gz", body);
}

// 110 events over several members: `a` on 100 of them, `b` on 10.
std::string write_budget_trace(const std::string& dir) {
    std::string body = "[\n";
    for (int i = 0; i < 110; ++i)
        body += event(i, "read",
                      i < 100 ? R"({"a":)" + std::to_string(i) + "}"
                              : R"({"b":)" + std::to_string(i) + "}");
    body += "]\n";
    const auto plain = dir + "/budget.pfw";
    {
        std::ofstream(plain) << body;
    }
    const auto gz = plain + ".gz";
    REQUIRE(
        dftu_utils_test::compress_file_to_gzip_multimember(plain, gz, 1024));
    fs::remove(plain);
    return gz;
}

index::IndexerOptions options(const std::string& index_dir,
                              std::size_t budget = 1024) {
    index::IndexerOptions o;
    o.index_dir = index_dir;
    o.bloom->path_budget = budget;
    return o;
}

std::map<std::string, PathStat> catalog_of(const std::string& trace,
                                           const std::string& index_path) {
    store::IndexDatabase db(index_path, store::IndexOpenMode::ReadOnly);
    const int fid =
        db.get_file_info_id(store::internal::get_logical_path(trace));
    REQUIRE(fid >= 0);
    std::map<std::string, PathStat> out;
    for (auto& [path, stat] : db.catalog(fid)) out.emplace(path, stat);
    return out;
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

TEST_SUITE("PathCatalog") {
    TEST_CASE("join follows the lattice") {
        using P = PathType;
        CHECK(store::join(P::INT, P::DOUBLE) == P::DOUBLE);
        CHECK(store::join(P::INT, P::UINT) == P::UINT);
        CHECK(store::join(P::UINT, P::DOUBLE) == P::DOUBLE);
        CHECK(store::join(P::INT, P::STRING) == P::MIXED);
        CHECK(store::join(P::BOOL, P::INT) == P::MIXED);
        CHECK(store::join(P::NULL_VALUE, P::STRING) == P::STRING);
        CHECK(store::join(P::MIXED, P::NULL_VALUE) == P::MIXED);
        const P all[] = {P::NULL_VALUE, P::BOOL,   P::INT,  P::UINT,
                         P::DOUBLE,     P::STRING, P::MIXED};
        for (P a : all)
            for (P b : all) {
                CHECK(store::join(a, b) == store::join(b, a));
                for (P c : all)
                    CHECK(store::join(store::join(a, b), c) ==
                          store::join(a, store::join(b, c)));
            }
    }

    TEST_CASE("a catalog record round-trips") {
        const PathStat s{PathType::UINT, 0x0c, 123456789012ULL};
        auto back =
            store::layout::decode_path_stat(store::layout::encode_path_stat(s));
        REQUIRE(back);
        CHECK(back->type == s.type);
        CHECK(back->seen == s.seen);
        CHECK(back->count == s.count);
        CHECK_FALSE(store::layout::decode_path_stat("x"));
    }

    TEST_CASE("the catalog lists every data path with its type and count") {
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = write_fixture(env.get_dir());
        const auto idx = env.get_dir() + "/idx";
        auto ix = index::Indexer::open({trace}, options(idx));
        const auto st = ix.build();
        const auto cat = catalog_of(trace, st.index_path);

        auto type_of = [&](const std::string& p) {
            REQUIRE_MESSAGE(cat.count(p), p);
            return cat.at(p).type;
        };
        CHECK(type_of("args.size") == PathType::INT);
        CHECK(type_of("args.ratio") == PathType::DOUBLE);
        CHECK(type_of("args.flag") == PathType::BOOL);
        CHECK(type_of("args.big") == PathType::UINT);
        CHECK(type_of("args.io.off") == PathType::INT);
        CHECK(type_of("args.io.mode") == PathType::STRING);
        CHECK(type_of("args.hosts.0") == PathType::STRING);
        CHECK(type_of("args.hosts.1") == PathType::STRING);
        CHECK(type_of("args.mix") == PathType::MIXED);
        CHECK(type_of("args.nul") == PathType::NULL_VALUE);
        CHECK(type_of("type") == PathType::STRING);
        CHECK(cat.at("args.size").count == 200);
        CHECK(cat.at("args.nul").count == 0);
        CHECK(cat.at("args.retry").count == 1);
        CHECK_FALSE(cat.count("args.value"));
        for (const char* axis : {"pid", "tid", "ts", "dur", "ph", "id"})
            CHECK_FALSE(cat.count(axis));

        bool listed = false;
        for (const auto& f : ix.manifest())
            for (const auto& e : f.extensions)
                if (e.name == "core.catalog") listed = e.current;
        CHECK(listed);
    }

    TEST_CASE("an empty array or object is listed and is not a column") {
        dftu_utils_test::TestEnvironment env(10);
        const std::string body = "[\n" +
                                 event(0, "read", R"({"tags":[],"opts":{}})") +
                                 event(1, "read", R"({"n":1})") + "]\n";
        const auto trace = dftu_utils_test::write_gz_trace(
            env.get_dir() + "/empty.pfw.gz", body);
        const auto idx = env.get_dir() + "/idx";
        const auto st = index::Indexer::open({trace}, options(idx)).build();
        const auto cat = catalog_of(trace, st.index_path);
        REQUIRE(cat.count("args.tags"));
        REQUIRE(cat.count("args.opts"));
        CHECK(cat.at("args.tags").type == PathType::ARRAY);
        CHECK(cat.at("args.opts").type == PathType::OBJECT);
        CHECK(cat.at("args.tags").count == 1);
        CHECK(cat.at("args.opts").count == 1);
        store::IndexDatabase db(st.index_path, store::IndexOpenMode::ReadOnly);
        std::map<std::string, ColumnType> cols;
        for (auto& [name, t] : db.query_all_column_types()) cols[name] = t;
        CHECK_FALSE(cols.count("tags"));
        CHECK_FALSE(cols.count("opts"));
        CHECK(cols.count("n"));
    }

    TEST_CASE("the schema folds every record of the catalog") {
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = write_fixture(env.get_dir());
        const auto idx = env.get_dir() + "/idx";
        const auto st = index::Indexer::open({trace}, options(idx)).build();
        store::IndexDatabase db(st.index_path, store::IndexOpenMode::ReadOnly);
        std::map<std::string, ColumnType> cols;
        for (auto& [name, t] : db.query_all_column_types()) cols[name] = t;
        CHECK(cols.at("size") == ColumnType::Int64);
        CHECK(cols.at("flag") == ColumnType::Int64);
        CHECK(cols.at("ratio") == ColumnType::Float64);
        CHECK(cols.at("big") == ColumnType::Float64);
        CHECK(cols.at("io.off") == ColumnType::Int64);
        CHECK(cols.at("hosts.1") == ColumnType::String);
        CHECK(cols.at("mix") == ColumnType::Json);
        CHECK(cols.at("retry") == ColumnType::Int64);
        CHECK(cols.at("type") == ColumnType::String);
        CHECK_FALSE(cols.count("nul"));
    }

    TEST_CASE("the path budget keeps the most frequent paths") {
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = write_budget_trace(env.get_dir());
        const auto idx = env.get_dir() + "/idx";
        const auto st = index::Indexer::open({trace}, options(idx, 1)).build();
        {
            store::IndexDatabase db(st.index_path,
                                    store::IndexOpenMode::ReadOnly);
            const int fid =
                db.get_file_info_id(store::internal::get_logical_path(trace));
            const auto zones = db.extension_paths(fid, IndexExtension::ZONEMAP);
            CHECK(std::find(zones.begin(), zones.end(), "a") != zones.end());
            CHECK(std::find(zones.begin(), zones.end(), "b") == zones.end());
            CHECK(db.catalog(fid).size() >= 3);
        }
        for (const char* q : {"b == 105", "b > 100", "a == 5"}) {
            CAPTURE(q);
            CHECK(count_rows(trace, idx, q, false) ==
                  count_rows(trace, idx, q, true));
        }
    }

    TEST_CASE("a changed budget rebuilds only the evidence it selects") {
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = write_budget_trace(env.get_dir());
        const auto idx = env.get_dir() + "/idx";
        auto before_ix = index::Indexer::open({trace}, options(idx, 1));
        before_ix.build();
        auto hashes = [&](const index::Indexer& ix) {
            std::map<std::string, std::uint64_t> out;
            for (const auto& f : ix.manifest())
                for (const auto& e : f.extensions) out[e.name] = e.params_hash;
            return out;
        };
        const auto before = hashes(before_ix);
        auto after_ix = index::Indexer::open({trace}, options(idx, 2));
        CHECK(after_ix.status().needs_work.size() == 1);
        CHECK(after_ix.build().indexed == 1);
        const auto after = hashes(after_ix);
        for (const char* changed : {"zonemap", "bloom", "counts"})
            CHECK(after.at(changed) != before.at(changed));
        for (const char* same : {"postings", "dft.stats", "core.catalog"})
            CHECK(after.at(same) == before.at(same));
        CHECK(after_ix.status().needs_work.empty());
    }
}
