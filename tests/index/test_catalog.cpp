#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/index/extensions/kinds/payloads.h>
#include <dftracer/utils/index/extensions/scalable_bloom_filter.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <random>
#include <set>
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

// `body` as `<name>.pfw.gz` over gzip members of about `member` bytes.
std::string write_members(const std::string& dir, const std::string& name,
                          const std::string& body, std::size_t member = 1024) {
    const auto plain = dir + "/" + name + ".pfw";
    {
        std::ofstream(plain) << body;
    }
    const auto gz = plain + ".gz";
    REQUIRE(
        dftu_utils_test::compress_file_to_gzip_multimember(plain, gz, member));
    fs::remove(plain);
    return gz;
}

// 110 events over several members: `a` on 100 of them, `b` on 10.
std::string write_budget_trace(const std::string& dir) {
    std::string body = "[\n";
    for (int i = 0; i < 110; ++i)
        body += event(i, "read",
                      i < 100 ? R"({"a":)" + std::to_string(i) + "}"
                              : R"({"b":)" + std::to_string(i) + "}");
    body += "]\n";
    return write_members(dir, "budget", body);
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

index::IndexerOptions options(const std::string& index_dir,
                              std::size_t budget = 0) {
    index::IndexerOptions o;
    o.index_dir = index_dir;
    o.bloom->path_budget = budget;
    return o;
}

struct Indexed {
    std::string trace;
    std::string index_path;
};

std::vector<std::string> zonemap_paths(const Indexed& t) {
    store::IndexDatabase db(t.index_path, store::IndexOpenMode::ReadOnly);
    const int fid =
        db.get_file_info_id(store::internal::get_logical_path(t.trace));
    REQUIRE(fid >= 0);
    return db.extension_paths(fid, IndexExtension::ZONEMAP);
}

bool has(const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

// The zones of `path`, by chunk.
std::map<std::uint64_t, index::extensions::kinds::Zone> zones_of(
    const Indexed& t, const std::string& path) {
    store::IndexDatabase db(t.index_path, store::IndexOpenMode::ReadOnly);
    const int fid =
        db.get_file_info_id(store::internal::get_logical_path(t.trace));
    std::map<std::uint64_t, index::extensions::kinds::Zone> out;
    for (auto& [chunk, bytes] :
         db.path_granules(fid, IndexExtension::ZONEMAP, path)) {
        auto zone = index::extensions::kinds::decode_zone(bytes);
        REQUIRE(zone);
        out.emplace(chunk, std::move(*zone));
    }
    return out;
}

std::set<std::uint64_t> read_chunks(const index::Indexer& ix,
                                    const std::string& query) {
    const auto e = ix.explain(query);
    REQUIRE(e.size() == 1);
    return {e[0].read.begin(), e[0].read.end()};
}

std::uint64_t chunk_count(const index::Indexer& ix) {
    return ix.explain(R"(name == "read")")[0].chunks;
}

void check_full_scan(const Indexed& t, const std::string& index_dir,
                     const std::vector<std::string>& queries) {
    for (const auto& q : queries) {
        CAPTURE(q);
        CHECK(count_rows(t.trace, index_dir, q, false) ==
              count_rows(t.trace, index_dir, q, true));
    }
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

        auto share = options(idx, 2);
        share.bloom->stats_share = 0.5;
        auto share_ix = index::Indexer::open({trace}, share);
        CHECK(share_ix.status().needs_work.size() == 1);
        CHECK(share_ix.build().indexed == 1);
        const auto shared = hashes(share_ix);
        for (const char* changed : {"zonemap", "bloom", "counts"})
            CHECK(shared.at(changed) != after.at(changed));
        for (const char* same : {"postings", "dft.stats", "core.catalog"})
            CHECK(shared.at(same) == after.at(same));
    }

    TEST_CASE("a stats share outside (0, 1] is an error naming it") {
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = write_budget_trace(env.get_dir());
        for (double bad : {0.0, -0.1, 1.5}) {
            CAPTURE(bad);
            auto o = options(env.get_dir() + "/idx");
            o.bloom->stats_share = bad;
            try {
                index::Indexer::open({trace}, o);
                FAIL("expected an error");
            } catch (const DFTUtilsException& e) {
                CHECK(e.code() == ErrorCode::INVALID_ARGUMENT);
                CHECK(std::string(e.what()).find("stats_share") !=
                      std::string::npos);
            }
        }
    }

    TEST_CASE("a wide file keeps evidence for every useful path") {
        dftu_utils_test::TestEnvironment env(10);
        // 400 paths; path p<j> is on the records of run j % 20 only, so
        // every path is absent from most chunks.
        std::string body = "[\n";
        for (int i = 0; i < 400; ++i) {
            std::string args = "{";
            for (int j = i / 20 % 20; j < 400; j += 20) {
                if (args.size() > 1) args += ",";
                args += "\"p" + std::to_string(1000 + j) +
                        "\":" + std::to_string(i);
            }
            body += event(i, "read", args + "}");
        }
        body += "]\n";
        const auto trace = write_members(env.get_dir(), "wide", body);
        const auto idx = env.get_dir() + "/idx";
        auto ix = index::Indexer::open({trace}, options(idx));
        const Indexed t{trace, ix.build().index_path};
        const auto zones = zonemap_paths(t);
        for (int j = 0; j < 400; ++j)
            CHECK_MESSAGE(has(zones, "p" + std::to_string(1000 + j)), j);
        const std::string q = "p1399 == 399";
        CHECK(read_chunks(ix, q).size() < chunk_count(ix));
        check_full_scan(t, idx, {q, "p1399 > 10", "exists(p1399)"});
    }

    TEST_CASE("evidence that cannot rule a chunk out is not kept") {
        dftu_utils_test::TestEnvironment env(10);
        // `c` is 7 and `u` "same" on every record, `u` over the distinct
        // limit in every chunk; `k` is 7 on the first half only; `a` varies
        // on 100 records.
        std::string body = "[\n";
        for (int i = 0; i < 110; ++i) {
            std::string args = R"({"c":7,"u":"same")";
            if (i < 55) args += R"(,"k":7)";
            if (i < 100) args += R"(,"a":)" + std::to_string(i);
            body += event(i, "read", args + "}");
        }
        body += "]\n";
        const auto trace = write_members(env.get_dir(), "useless", body);
        for (std::size_t budget : {std::size_t{0}, std::size_t{1}}) {
            CAPTURE(budget);
            const auto idx = env.get_dir() + "/idx" + std::to_string(budget);
            auto o = options(idx, budget);
            o.bloom->auto_max_distinct = 0;
            auto ix = index::Indexer::open({trace}, o);
            const Indexed t{trace, ix.build().index_path};
            const auto zones = zonemap_paths(t);
            CHECK_FALSE(has(zones, "c"));
            CHECK_FALSE(has(zones, "u"));
            CHECK(zones_of(t, "c").empty());
            CHECK(zones_of(t, "u").empty());
            CHECK(has(zones, "a"));
            CHECK(has(zones, "k") == (budget == 0));
            check_full_scan(t, idx,
                            {"c == 7", "c > 7", R"(u == "same")", "k == 7",
                             "a == 5", "exists(k)"});
            if (budget != 0) continue;
            const auto k = zones_of(t, "k");
            REQUIRE_FALSE(k.empty());
            for (const auto& [chunk, zone] : k) {
                CAPTURE(chunk);
                REQUIRE(zone.present);
                CHECK(*zone.present == 0);
            }
            const auto read = read_chunks(ix, "k == 7");
            CHECK(read.size() < chunk_count(ix));
            for (const auto& [chunk, zone] : k) CHECK_FALSE(read.count(chunk));
        }
    }

    TEST_CASE("the cap keeps the most frequent paths whose evidence fits") {
        dftu_utils_test::TestEnvironment env(10);
        // 1400 paths in groups of 200: group g is on every (g + 1)th record,
        // so ranking is by group, then by name; values differ by record.
        constexpr int PATHS = 1400;
        std::string body = "[\n";
        for (int i = 0; i < 200; ++i) {
            std::string args = "{";
            for (int j = 0; j < PATHS; ++j) {
                if (i % (1 + j / 200) != 0) continue;
                if (args.size() > 1) args += ",";
                args += "\"q" + std::to_string(10000 + j) +
                        "\":" + std::to_string(i * 10000 + j);
            }
            body += event(i, "read", args + "}");
        }
        body += "]\n";
        const auto trace = write_members(env.get_dir(), "capped", body, 4096);
        auto build = [&](const std::string& idx) {
            auto ix = index::Indexer::open({trace}, options(idx));
            return Indexed{trace, ix.build().index_path};
        };
        const auto t = build(env.get_dir() + "/idx");
        const auto zones = zonemap_paths(t);
        const auto cat = catalog_of(trace, t.index_path);
        std::vector<std::string> kept;
        std::vector<std::string> dropped;
        for (int j = 0; j < PATHS; ++j) {
            const std::string p = "q" + std::to_string(10000 + j);
            (has(zones, p) ? kept : dropped).push_back(p);
        }
        REQUIRE_FALSE(kept.empty());
        REQUIRE_FALSE(dropped.empty());
        auto rank = [&](const std::string& p) {
            return std::make_pair(
                -static_cast<std::int64_t>(cat.at("args." + p).count), p);
        };
        std::pair<std::int64_t, std::string> worst_kept = rank(kept.front());
        for (const auto& p : kept) worst_kept = std::max(worst_kept, rank(p));
        for (const auto& p : dropped) CHECK(rank(p) > worst_kept);

        const auto again = build(env.get_dir() + "/idx_again");
        CHECK(zonemap_paths(again) == zones);
        for (const auto& p : {kept.front(), kept.back()})
            CHECK(zones_of(again, p).size() == zones_of(t, p).size());
        check_full_scan(
            t, env.get_dir() + "/idx",
            {kept.back() + " == 0", dropped.front() + " > 5",
             dropped.back() + " == " + std::to_string(10000 + PATHS - 1)});
    }
}

TEST_SUITE("Absence") {
    TEST_CASE("a chunk without the path is skipped for a test on its value") {
        dftu_utils_test::TestEnvironment env(10);
        // `x` on the first 50 records only.
        std::string body = "[\n";
        for (int i = 0; i < 100; ++i) {
            std::string args = R"({"y":)" + std::to_string(i) + R"(,"m":"m)" +
                               std::to_string(i / 10 % 10) + "_" +
                               std::to_string(i % 3) + "\"";
            if (i < 50) args += R"(,"x":)" + std::to_string(i);
            body += event(i, "read", args + "}");
        }
        body += "]\n";
        const auto trace = write_members(env.get_dir(), "absent", body);
        const auto idx = env.get_dir() + "/idx";
        auto ix = index::Indexer::open({trace}, options(idx));
        const Indexed t{trace, ix.build().index_path};
        const auto all = chunk_count(ix);
        std::set<std::uint64_t> with_x;
        for (const auto& [chunk, zone] : zones_of(t, "x")) {
            REQUIRE(zone.present);
            if (*zone.present > 0) with_x.insert(chunk);
        }
        REQUIRE_FALSE(with_x.empty());
        REQUIRE(with_x.size() < all);

        for (const char* q : {"x == 1", "x >= 0", "x in [1, 2]", "exists(x)"}) {
            CAPTURE(q);
            const auto read = read_chunks(ix, q);
            CHECK_FALSE(read.empty());
            for (auto c : read) CHECK(with_x.count(c));
        }
        for (const char* q : {"not exists(x)", "x is null", "x != 1",
                              "not (x == 1)", "y >= 0"}) {
            CAPTURE(q);
            CHECK(read_chunks(ix, q).size() == all);
        }
        check_full_scan(
            t, idx,
            {"x == 1", "x >= 0", "x in [1, 2]", "exists(x)", "not exists(x)",
             "x is null", "x is not null", "x != 1", "not (x == 1)",
             "x == 1 or y == 70", R"(m == "m7_1")"});
    }

    TEST_CASE("a chunk bloom is sized for the chunk's values") {
        dftu_utils_test::TestEnvironment env(10);
        std::string body = "[\n";
        for (int i = 0; i < 100; ++i)
            body +=
                event(i, "read", R"({"m":"v)" + std::to_string(i % 3) + "\"}");
        body += "]\n";
        const auto trace = write_members(env.get_dir(), "small", body);
        const auto idx = env.get_dir() + "/idx";
        const Indexed t{
            trace,
            index::Indexer::open({trace}, options(idx)).build().index_path};
        store::IndexDatabase db(t.index_path, store::IndexOpenMode::ReadOnly);
        const int fid =
            db.get_file_info_id(store::internal::get_logical_path(trace));
        const auto blooms = db.path_granules(fid, IndexExtension::BLOOM, "m");
        REQUIRE(blooms.size() > 1);
        const index::extensions::ScalableBloomFilter configured(1024, 0.01);
        const index::extensions::ScalableBloomFilter three(4, 0.01);
        bool full = false;
        for (const auto& [chunk, bytes] : blooms) {
            CAPTURE(chunk);
            auto bloom = index::extensions::kinds::decode_bloom(bytes);
            REQUIRE(bloom);
            CHECK(bloom->num_entries() <= 3);
            CHECK(bloom->num_levels() == 1);
            CHECK(bloom->size_bytes() == three.size_bytes());
            CHECK(bloom->size_bytes() < configured.size_bytes());
            full = full || bloom->num_entries() == 3;
        }
        CHECK(full);
    }

    TEST_CASE("pruned reads equal full scans over random filters") {
        dftu_utils_test::TestEnvironment env(10);
        // Runs of records carry `x` (a number), `s` (a string), `n` (a
        // number or null), `o` (a number or an object) and `e` (a number or
        // an empty object), each on some runs only.
        std::mt19937 rng(42);
        std::string body = "[\n";
        for (int i = 0; i < 300; ++i) {
            const int run = i / 25;
            std::string args = R"({"i":)" + std::to_string(i);
            if (run % 2 == 0) args += R"(,"x":)" + std::to_string(rng() % 20);
            if (run % 3 == 0)
                args += R"(,"s":"s)" + std::to_string(rng() % 6) + "\"";
            if (run % 4 == 1)
                args += i % 7 == 0 ? R"(,"n":null)"
                                   : R"(,"n":)" + std::to_string(rng() % 9);
            if (run == 5) args += R"(,"o":{"k":1})";
            if (run == 7) args += R"(,"o":)" + std::to_string(rng() % 4);
            if (run == 9) args += R"(,"e":{})";
            if (run == 10) args += R"(,"e":)" + std::to_string(rng() % 4);
            body += event(i, "read", args + "}");
        }
        body += "]\n";
        const auto trace = write_members(env.get_dir(), "random", body);
        const auto idx = env.get_dir() + "/idx";
        const Indexed t{
            trace,
            index::Indexer::open({trace}, options(idx)).build().index_path};
        std::vector<std::string> queries;
        for (const char* p : {"x", "s", "n", "o", "e", "missing"}) {
            const std::string f(p);
            queries.push_back("exists(" + f + ")");
            queries.push_back("not exists(" + f + ")");
            queries.push_back(f + " is null");
            queries.push_back(f + " is not null");
        }
        for (int k = 0; k < 60; ++k) {
            const char* p = std::vector<const char*>{"x", "n", "o", "e"}[k % 4];
            const auto v = std::to_string(rng() % 20);
            const char* ops[] = {" == ", " != ", " > ", " <= "};
            queries.push_back(std::string(p) + ops[rng() % 4] + v);
            queries.push_back("not (" + std::string(p) + " == " + v + ")");
            queries.push_back(std::string(p) + " in [" + v + ", 3]");
            queries.push_back(R"(s == "s)" + std::to_string(rng() % 7) +
                              "\" or " + p + " < " + v);
        }
        check_full_scan(t, idx, queries);
    }
}

TEST_SUITE("TopLevelShadow") {
    // Records carry a top-level field and an args key of the same name with
    // other values; an unprefixed query reads the top-level value first, so
    // evidence built from the args key must not prune it.
    TEST_CASE("an args key named like a top-level field does not prune it") {
        dftu_utils_test::TestEnvironment env(10);
        std::string body = "[\n";
        for (int i = 0; i < 200; ++i) {
            std::ostringstream o;
            o << R"({"id":)" << i << R"(,"name":"read","cat":"POSIX")"
              << R"(,"pid":1,"tid":2,"ph":"X","ts":)" << 1000 + i
              << R"(,"dur":5,"type":"x","foo":"top)" << i
              << R"(","args":{"id":)" << 100000 + i << R"(,"cat":"ARG)" << i
              << R"(","type":"arg)" << i << R"(","foo":"arg)" << i << R"("}})"
              << "\n";
            body += o.str();
        }
        body += "]\n";
        const auto trace = write_members(env.get_dir(), "shadow", body);
        const auto idx = env.get_dir() + "/idx";
        auto ix = index::Indexer::open({trace}, options(idx));
        const Indexed t{trace, ix.build().index_path};
        check_full_scan(
            t, idx,
            {R"(cat == "POSIX")", "id == 5", "id > 150", "id < 50",
             R"(type == "x")", R"(foo == "top7")", R"(foo in ["top1", "top2"])",
             "exists(foo)", "args.id == 100005", R"(args.foo == "arg7")"});
        for (const char* q : {"args.id == 100005", R"(args.foo == "arg7")"}) {
            const std::string query(q);
            CAPTURE(query);
            CHECK(read_chunks(ix, query).size() < chunk_count(ix));
        }
    }
}
