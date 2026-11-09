// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/extensions/kinds/payloads.h>
#include <dftracer/utils/index/extensions/scalable_bloom_filter.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <algorithm>
#include <random>

using namespace dftracer::utils;

namespace {

dstd::String event(int i, const dstd::String& name, const dstd::String& args) {
    dstd::String o;
    o += common::str_cat(R"({"id":)", i, R"(,"name":")", name,
                         R"(","cat":"POSIX","pid":1,"tid":2,"ph":"X","ts":)",
                         1000 + i, R"(,"dur":5,"type":"x","args":)", args,
                         "}\n");
    return o;
}

// 200 events: nested, array, bool, big uint, double and null args; `mix` is a
// number on the first 100 events and a string after; only the last carries
// `retry`. A metadata record carries args the catalog must not list.
dstd::String write_fixture(const dstd::String& dir) {
    dstd::String body = "[\n";
    body +=
        R"({"name":"FH","ph":"M","pid":1,"tid":1,"args":{"name":"/a","value":"abc"}})"
        "\n";
    for (int i = 0; i < 200; ++i) {
        dstd::String a;
        a += common::str_cat(
            R"({"size":)", i * 10, R"(,"ratio":)", i, ".5", R"(,"flag":)",
            (i % 2 ? "true" : "false"),
            R"(,"big":18446744073709551000,"io":{"off":)", i,
            R"(,"mode":"r"},"hosts":["h1","h2"],"nul":null,"mix":)");
        if (i < 100)
            a += common::str_cat(i);
        else
            a += R"("s")";
        if (i == 199) a += R"(,"retry":1)";
        a += "}";
        body += event(i, i % 3 ? "read" : "write", a);
    }
    body += "]\n";
    return dftu_utils_test::write_gz_trace(dir + "/fixture.pfw.gz", body);
}

// `body` as `<name>.pfw.gz` over gzip members of about `member` bytes.
dstd::String write_members(const dstd::String& dir, const dstd::String& name,
                           const dstd::String& body,
                           std::size_t member = 1024) {
    const auto plain = dir + "/" + name + ".pfw";
    {
        dstd::fs::write_file(dstd::PathView(plain), body).discard();
    }
    const auto gz = plain + ".gz";
    REQUIRE(
        dftu_utils_test::compress_file_to_gzip_multimember(plain, gz, member));
    dstd::fs::remove(plain).discard();
    return gz;
}

// 110 events over several members: `a` on 100 of them, `b` on 10.
dstd::String write_budget_trace(const dstd::String& dir) {
    dstd::String body = "[\n";
    for (int i = 0; i < 110; ++i)
        body += event(i, "read",
                      i < 100 ? R"({"a":)" + dstd::to_string(i) + "}"
                              : R"({"b":)" + dstd::to_string(i) + "}");
    body += "]\n";
    return write_members(dir, "budget", body);
}

std::size_t count_rows(const dstd::String& trace, const dstd::String& index_dir,
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
    return dftu_utils_test::ok(run().get());
}

index::IndexerOptions options(const dstd::String& index_dir,
                              std::size_t budget = 0) {
    index::IndexerOptions o;
    o.index_dir = index_dir;
    o.bloom->path_budget = budget;
    return o;
}

struct Indexed {
    dstd::String trace;
    dstd::String index_path;
};

dstd::Vector<dstd::String> zonemap_paths(const Indexed& t) {
    const auto snapshot = dftu_utils_test::index_snapshot(t.index_path);
    const auto file = dftu_utils_test::require_indexed_file(snapshot, t.trace);
    dstd::Vector<dstd::String> out;
    for (const auto& path : dftu_utils_test::index_take(index::paths(
             snapshot, file, index::store::IndexExtension::Zonemap)))
        out.emplace_back(path.data(), path.size());
    return out;
}

bool has(const dstd::Vector<dstd::String>& v, const dstd::String& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

// The zones of `path`, by chunk.
dstd::Map<std::uint64_t, index::extensions::kinds::Zone> zones_of(
    const Indexed& t, const dstd::String& path) {
    const auto snapshot = dftu_utils_test::index_snapshot(t.index_path);
    const auto file = dftu_utils_test::require_indexed_file(snapshot, t.trace);
    dstd::Map<std::uint64_t, index::extensions::kinds::Zone> out;
    for (auto& [chunk, bytes] : dftu_utils_test::index_take(index::granules(
             snapshot, file, index::store::IndexExtension::Zonemap, path))) {
        auto zone = index::extensions::kinds::decode_zone(
            dstd::StringView(bytes.data(), bytes.size()));
        REQUIRE(zone);
        out.emplace(chunk, std::move(*zone));
    }
    return out;
}

dstd::Set<std::uint64_t> read_chunks(const index::Indexer& ix,
                                     const dstd::String& query) {
    const auto e = ix.explain(query);
    REQUIRE(e.size() == 1);
    return {e[0].read.begin(), e[0].read.end()};
}

std::uint64_t chunk_count(const index::Indexer& ix) {
    return ix.explain(R"(name == "read")")[0].chunks;
}

void check_full_scan(const Indexed& t, const dstd::String& index_dir,
                     const dstd::Vector<dstd::String>& queries) {
    for (const auto& q : queries) {
        CAPTURE(q);
        CHECK(count_rows(t.trace, index_dir, q, false) ==
              count_rows(t.trace, index_dir, q, true));
    }
}

dstd::Map<dstd::String, index::store::PathStat> catalog_of(
    const dstd::String& trace, dstd::StringView index_path) {
    const auto snapshot = dftu_utils_test::index_snapshot(index_path);
    const auto file = dftu_utils_test::require_indexed_file(snapshot, trace);
    dstd::Map<dstd::String, index::store::PathStat> out;
    for (auto& [path, stat] :
         dftu_utils_test::index_take(index::catalog(snapshot, file)))
        out.emplace(dstd::String(path.data(), path.size()), stat);
    return out;
}

// The column types of the file's catalog, without the "args." prefix.
dstd::Map<dstd::String, index::store::ColumnType> column_types(
    const dstd::String& trace, dstd::StringView index_path) {
    constexpr dstd::StringView ARGS = "args.";
    const auto snapshot = dftu_utils_test::index_snapshot(index_path);
    const auto file = dftu_utils_test::require_indexed_file(snapshot, trace);
    dstd::Map<dstd::String, index::store::ColumnType> out;
    for (auto& [name, t] : dftu_utils_test::index_take(
             index::file_column_types(snapshot, file))) {
        dstd::StringView path(name.data(), name.size());
        if (path.starts_with(ARGS)) path.remove_prefix(ARGS.size());
        out[dstd::String(path)] = t;
    }
    return out;
}

}  // namespace

TEST_SUITE("PathCatalog") {
    TEST_CASE("join follows the lattice") {
        using P = index::store::PathType;
        CHECK(index::store::join(P::Int, P::Double) == P::Double);
        CHECK(index::store::join(P::Int, P::Uint) == P::Uint);
        CHECK(index::store::join(P::Uint, P::Double) == P::Double);
        CHECK(index::store::join(P::Int, P::String) == P::Mixed);
        CHECK(index::store::join(P::Bool, P::Int) == P::Mixed);
        CHECK(index::store::join(P::NullValue, P::String) == P::String);
        CHECK(index::store::join(P::Mixed, P::NullValue) == P::Mixed);
        const P all[] = {P::NullValue, P::Bool,   P::Int,  P::Uint,
                         P::Double,    P::String, P::Mixed};
        for (P a : all)
            for (P b : all) {
                CHECK(index::store::join(a, b) == index::store::join(b, a));
                for (P c : all)
                    CHECK(index::store::join(index::store::join(a, b), c) ==
                          index::store::join(a, index::store::join(b, c)));
            }
    }

    TEST_CASE("a catalog record round-trips") {
        const index::store::PathStat s{index::store::PathType::Uint, 0x0c,
                                       123456789012ULL};
        auto back = index::store::layout::decode_path_stat(
            index::store::layout::encode_path_stat(s));
        REQUIRE(back);
        CHECK(back->type == s.type);
        CHECK(back->seen == s.seen);
        CHECK(back->count == s.count);
        CHECK_FALSE(index::store::layout::decode_path_stat("x"));
    }

    TEST_CASE("the catalog lists every data path with its type and count") {
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = write_fixture(env.dir());
        const auto idx = env.dir() + "/idx";
        auto ix = index::Indexer::open({dstd::String(dstd::StringView(trace))},
                                       options(idx));
        const auto st = ix.build();
        const auto cat = catalog_of(trace, st.index_path);

        auto type_of = [&](const dstd::String& p) {
            const dstd::String key(p.data(), p.size());
            REQUIRE_MESSAGE(cat.count(key), p);
            return cat.at(key).type;
        };
        CHECK(type_of("args.size") == index::store::PathType::Int);
        CHECK(type_of("args.ratio") == index::store::PathType::Double);
        CHECK(type_of("args.flag") == index::store::PathType::Bool);
        CHECK(type_of("args.big") == index::store::PathType::Uint);
        CHECK(type_of("args.io.off") == index::store::PathType::Int);
        CHECK(type_of("args.io.mode") == index::store::PathType::String);
        CHECK(type_of("args.hosts.0") == index::store::PathType::String);
        CHECK(type_of("args.hosts.1") == index::store::PathType::String);
        CHECK(type_of("args.mix") == index::store::PathType::Mixed);
        CHECK(type_of("args.nul") == index::store::PathType::NullValue);
        CHECK(type_of("type") == index::store::PathType::String);
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
        const dstd::String body = "[\n" +
                                  event(0, "read", R"({"tags":[],"opts":{}})") +
                                  event(1, "read", R"({"n":1})") + "]\n";
        const auto trace =
            dftu_utils_test::write_gz_trace(env.dir() + "/empty.pfw.gz", body);
        const auto idx = env.dir() + "/idx";
        const auto st =
            index::Indexer::open({dstd::String(dstd::StringView(trace))},
                                 options(idx))
                .build();
        const auto cat = catalog_of(trace, st.index_path);
        REQUIRE(cat.count("args.tags"));
        REQUIRE(cat.count("args.opts"));
        CHECK(cat.at("args.tags").type == index::store::PathType::Array);
        CHECK(cat.at("args.opts").type == index::store::PathType::Object);
        CHECK(cat.at("args.tags").count == 1);
        CHECK(cat.at("args.opts").count == 1);
        const auto cols = column_types(trace, st.index_path);
        CHECK_FALSE(cols.count("tags"));
        CHECK_FALSE(cols.count("opts"));
        CHECK(cols.count("n"));
    }

    TEST_CASE("the schema folds every record of the catalog") {
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = write_fixture(env.dir());
        const auto idx = env.dir() + "/idx";
        const auto st =
            index::Indexer::open({dstd::String(dstd::StringView(trace))},
                                 options(idx))
                .build();
        const auto cols = column_types(trace, st.index_path);
        CHECK(cols.at("size") == index::store::ColumnType::Int64);
        CHECK(cols.at("flag") == index::store::ColumnType::Int64);
        CHECK(cols.at("ratio") == index::store::ColumnType::Float64);
        CHECK(cols.at("big") == index::store::ColumnType::Float64);
        CHECK(cols.at("io.off") == index::store::ColumnType::Int64);
        CHECK(cols.at("hosts.1") == index::store::ColumnType::String);
        CHECK(cols.at("mix") == index::store::ColumnType::Json);
        CHECK(cols.at("retry") == index::store::ColumnType::Int64);
        CHECK(cols.at("type") == index::store::ColumnType::String);
        CHECK_FALSE(cols.count("nul"));
    }

    TEST_CASE("the path budget keeps the most frequent paths") {
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = write_budget_trace(env.dir());
        const auto idx = env.dir() + "/idx";
        const auto st =
            index::Indexer::open({dstd::String(dstd::StringView(trace))},
                                 options(idx, 1))
                .build();
        {
            const Indexed built{trace, st.index_path};
            const auto zones = zonemap_paths(built);
            CHECK(std::find(zones.begin(), zones.end(), "a") != zones.end());
            CHECK(std::find(zones.begin(), zones.end(), "b") == zones.end());
            CHECK(catalog_of(trace, st.index_path).size() >= 3);
        }
        for (const char* q : {"b == 105", "b > 100", "a == 5"}) {
            CAPTURE(q);
            CHECK(count_rows(trace, idx, q, false) ==
                  count_rows(trace, idx, q, true));
        }
    }

    TEST_CASE("a changed budget rebuilds only the evidence it selects") {
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = write_budget_trace(env.dir());
        const auto idx = env.dir() + "/idx";
        auto before_ix = index::Indexer::open(
            {dstd::String(dstd::StringView(trace))}, options(idx, 1));
        before_ix.build();
        auto hashes = [&](const index::Indexer& ix) {
            dstd::Map<dstd::String, std::uint64_t> out;
            for (const auto& f : ix.manifest())
                for (const auto& e : f.extensions) out[e.name] = e.params_hash;
            return out;
        };
        const auto before = hashes(before_ix);
        auto after_ix = index::Indexer::open(
            {dstd::String(dstd::StringView(trace))}, options(idx, 2));
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
        auto share_ix = index::Indexer::open(
            {dstd::String(dstd::StringView(trace))}, share);
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
        const auto trace = write_budget_trace(env.dir());
        for (double bad : {0.0, -0.1, 1.5}) {
            CAPTURE(bad);
            auto o = options(env.dir() + "/idx");
            o.bloom->stats_share = bad;
            try {
                index::Indexer::open({dstd::String(dstd::StringView(trace))},
                                     o);
                FAIL("expected an error");
            } catch (const common::DFTUtilsException& e) {
                CHECK(e.error().is_code(common::ErrorCode::InvalidArgument));
                CHECK(dstd::String(e.what()).find("stats_share") !=
                      dstd::String::npos);
            }
        }
    }

    TEST_CASE("a wide file keeps evidence for every useful path") {
        dftu_utils_test::TestEnvironment env(10);
        // 400 paths; path p<j> is on the records of run j % 20 only, so
        // every path is absent from most chunks.
        dstd::String body = "[\n";
        for (int i = 0; i < 400; ++i) {
            dstd::String args = "{";
            for (int j = i / 20 % 20; j < 400; j += 20) {
                if (args.size() > 1) args += ",";
                args += "\"p" + dstd::to_string(1000 + j) +
                        "\":" + dstd::to_string(i);
            }
            body += event(i, "read", args + "}");
        }
        body += "]\n";
        const auto trace = write_members(env.dir(), "wide", body);
        const auto idx = env.dir() + "/idx";
        auto ix = index::Indexer::open({dstd::String(dstd::StringView(trace))},
                                       options(idx));
        const Indexed t{trace, ix.build().index_path};
        const auto zones = zonemap_paths(t);
        for (int j = 0; j < 400; ++j)
            CHECK_MESSAGE(has(zones, "p" + dstd::to_string(1000 + j)), j);
        const dstd::String q = "p1399 == 399";
        CHECK(read_chunks(ix, q).size() < chunk_count(ix));
        check_full_scan(t, idx, {q, "p1399 > 10", "exists(p1399)"});
    }

    TEST_CASE("evidence that cannot rule a chunk out is not kept") {
        dftu_utils_test::TestEnvironment env(10);
        // `c` is 7 and `u` "same" on every record, `u` over the distinct
        // limit in every chunk; `k` is 7 on the first half only; `a` varies
        // on 100 records.
        dstd::String body = "[\n";
        for (int i = 0; i < 110; ++i) {
            dstd::String args = R"({"c":7,"u":"same")";
            if (i < 55) args += R"(,"k":7)";
            if (i < 100) args += R"(,"a":)" + dstd::to_string(i);
            body += event(i, "read", args + "}");
        }
        body += "]\n";
        const auto trace = write_members(env.dir(), "useless", body);
        for (std::size_t budget : {std::size_t{0}, std::size_t{1}}) {
            CAPTURE(budget);
            const auto idx = env.dir() + "/idx" + dstd::to_string(budget);
            auto o = options(idx, budget);
            o.bloom->auto_max_distinct = 0;
            auto ix = index::Indexer::open(
                {dstd::String(dstd::StringView(trace))}, o);
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
        dstd::String body = "[\n";
        for (int i = 0; i < 200; ++i) {
            dstd::String args = "{";
            for (int j = 0; j < PATHS; ++j) {
                if (i % (1 + j / 200) != 0) continue;
                if (args.size() > 1) args += ",";
                args += "\"q" + dstd::to_string(10000 + j) +
                        "\":" + dstd::to_string(i * 10000 + j);
            }
            body += event(i, "read", args + "}");
        }
        body += "]\n";
        const auto trace = write_members(env.dir(), "capped", body, 4096);
        auto build = [&](const dstd::String& idx) {
            auto ix = index::Indexer::open(
                {dstd::String(dstd::StringView(trace))}, options(idx));
            return Indexed{trace, ix.build().index_path};
        };
        const auto t = build(env.dir() + "/idx");
        const auto zones = zonemap_paths(t);
        const auto cat = catalog_of(trace, t.index_path);
        dstd::Vector<dstd::String> kept;
        dstd::Vector<dstd::String> dropped;
        for (int j = 0; j < PATHS; ++j) {
            const dstd::String p = "q" + dstd::to_string(10000 + j);
            (has(zones, p) ? kept : dropped).push_back(p);
        }
        REQUIRE_FALSE(kept.empty());
        REQUIRE_FALSE(dropped.empty());
        auto rank = [&](const dstd::String& p) {
            return dstd::Pair<std::int64_t, dstd::String>{
                -static_cast<std::int64_t>(
                    cat.at(dstd::String("args." + p)).count),
                p};
        };
        dstd::Pair<std::int64_t, dstd::String> worst_kept = rank(kept.front());
        for (const auto& p : kept) worst_kept = std::max(worst_kept, rank(p));
        for (const auto& p : dropped) CHECK(rank(p) > worst_kept);

        const auto again = build(env.dir() + "/idx_again");
        CHECK(zonemap_paths(again) == zones);
        for (const auto& p : {kept.front(), kept.back()})
            CHECK(zones_of(again, p).size() == zones_of(t, p).size());
        check_full_scan(
            t, env.dir() + "/idx",
            {kept.back() + " == 0", dropped.front() + " > 5",
             dropped.back() + " == " + dstd::to_string(10000 + PATHS - 1)});
    }
}

TEST_SUITE("Absence") {
    TEST_CASE("a chunk without the path is skipped for a test on its value") {
        dftu_utils_test::TestEnvironment env(10);
        // `x` on the first 50 records only.
        dstd::String body = "[\n";
        for (int i = 0; i < 100; ++i) {
            dstd::String args = R"({"y":)" + dstd::to_string(i) + R"(,"m":"m)" +
                                dstd::to_string(i / 10 % 10) + "_" +
                                dstd::to_string(i % 3) + "\"";
            if (i < 50) args += R"(,"x":)" + dstd::to_string(i);
            body += event(i, "read", args + "}");
        }
        body += "]\n";
        const auto trace = write_members(env.dir(), "absent", body);
        const auto idx = env.dir() + "/idx";
        auto ix = index::Indexer::open({dstd::String(dstd::StringView(trace))},
                                       options(idx));
        const Indexed t{trace, ix.build().index_path};
        const auto all = chunk_count(ix);
        dstd::Set<std::uint64_t> with_x;
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
        dstd::String body = "[\n";
        for (int i = 0; i < 100; ++i)
            body +=
                event(i, "read", R"({"m":"v)" + dstd::to_string(i % 3) + "\"}");
        body += "]\n";
        const auto trace = write_members(env.dir(), "small", body);
        const auto idx = env.dir() + "/idx";
        const Indexed t{
            trace, index::Indexer::open({dstd::String(dstd::StringView(trace))},
                                        options(idx))
                       .build()
                       .index_path};
        const auto snapshot = dftu_utils_test::index_snapshot(t.index_path);
        const auto file =
            dftu_utils_test::require_indexed_file(snapshot, trace);
        const auto blooms = dftu_utils_test::index_take(index::granules(
            snapshot, file, index::store::IndexExtension::Bloom, "m"));
        REQUIRE(blooms.size() > 1);
        const index::extensions::ScalableBloomFilter configured(1024, 0.01);
        const index::extensions::ScalableBloomFilter three(4, 0.01);
        bool full = false;
        for (const auto& [chunk, bytes] : blooms) {
            CAPTURE(chunk);
            auto bloom = index::extensions::kinds::decode_bloom(
                dstd::StringView(bytes.data(), bytes.size()));
            REQUIRE(bloom);
            CHECK(bloom->entry_count() <= 3);
            CHECK(bloom->level_count() == 1);
            CHECK(bloom->size_bytes() == three.size_bytes());
            CHECK(bloom->size_bytes() < configured.size_bytes());
            full = full || bloom->entry_count() == 3;
        }
        CHECK(full);
    }

    TEST_CASE("pruned reads equal full scans over random filters") {
        dftu_utils_test::TestEnvironment env(10);
        // Runs of records carry `x` (a number), `s` (a string), `n` (a
        // number or null), `o` (a number or an object) and `e` (a number or
        // an empty object), each on some runs only.
        std::mt19937 rng(42);
        dstd::String body = "[\n";
        for (int i = 0; i < 300; ++i) {
            const int run = i / 25;
            dstd::String args = R"({"i":)" + dstd::to_string(i);
            if (run % 2 == 0) args += R"(,"x":)" + dstd::to_string(rng() % 20);
            if (run % 3 == 0)
                args += R"(,"s":"s)" + dstd::to_string(rng() % 6) + "\"";
            if (run % 4 == 1)
                args += i % 7 == 0 ? R"(,"n":null)"
                                   : R"(,"n":)" + dstd::to_string(rng() % 9);
            if (run == 5) args += R"(,"o":{"k":1})";
            if (run == 7) args += R"(,"o":)" + dstd::to_string(rng() % 4);
            if (run == 9) args += R"(,"e":{})";
            if (run == 10) args += R"(,"e":)" + dstd::to_string(rng() % 4);
            body += event(i, "read", args + "}");
        }
        body += "]\n";
        const auto trace = write_members(env.dir(), "random", body);
        const auto idx = env.dir() + "/idx";
        const Indexed t{
            trace, index::Indexer::open({dstd::String(dstd::StringView(trace))},
                                        options(idx))
                       .build()
                       .index_path};
        dstd::Vector<dstd::String> queries;
        for (const char* p : {"x", "s", "n", "o", "e", "missing"}) {
            const dstd::String f(p);
            queries.push_back("exists(" + f + ")");
            queries.push_back("not exists(" + f + ")");
            queries.push_back(f + " is null");
            queries.push_back(f + " is not null");
        }
        for (int k = 0; k < 60; ++k) {
            const char* p =
                dstd::Vector<const char*>{"x", "n", "o", "e"}[k % 4];
            const auto v = dstd::to_string(rng() % 20);
            const char* ops[] = {" == ", " != ", " > ", " <= "};
            queries.push_back(dstd::String(p) + ops[rng() % 4] + v);
            queries.push_back("not (" + dstd::String(p) + " == " + v + ")");
            queries.push_back(dstd::String(p) + " in [" + v + ", 3]");
            queries.push_back(R"(s == "s)" + dstd::to_string(rng() % 7) +
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
        dstd::String body = "[\n";
        for (int i = 0; i < 200; ++i) {
            dstd::String o;
            o += common::str_cat(
                R"({"id":)", i, R"(,"name":"read","cat":"POSIX")",
                R"(,"pid":1,"tid":2,"ph":"X","ts":)", 1000 + i,
                R"(,"dur":5,"type":"x","foo":"top)", i, R"(","args":{"id":)",
                100000 + i, R"(,"cat":"ARG)", i, R"(","type":"arg)", i,
                R"(","foo":"arg)", i, R"("}})", "\n");
            body += o;
        }
        body += "]\n";
        const auto trace = write_members(env.dir(), "shadow", body);
        const auto idx = env.dir() + "/idx";
        auto ix = index::Indexer::open({dstd::String(dstd::StringView(trace))},
                                       options(idx));
        const Indexed t{trace, ix.build().index_path};
        check_full_scan(
            t, idx,
            {R"(cat == "POSIX")", "id == 5", "id > 150", "id < 50",
             R"(type == "x")", R"(foo == "top7")", R"(foo in ["top1", "top2"])",
             "exists(foo)", "args.id == 100005", R"(args.foo == "arg7")"});
        for (const char* q : {"args.id == 100005", R"(args.foo == "arg7")"}) {
            const dstd::String query(q);
            CAPTURE(query);
            CHECK(read_chunks(ix, query).size() < chunk_count(ix));
        }
    }
}
