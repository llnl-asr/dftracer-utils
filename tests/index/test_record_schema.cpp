// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <refused_lines.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <algorithm>

using namespace dftracer::utils;
using namespace dftracer::utils::index;

namespace {

const char* OPS[] = {"read", "write", "open", "close"};

// Generic NDJSON over small gzip members: `op` is constant for runs of 400
// records, so chunks hold disjoint ops; `io.off` grows with the record.
dstd::String write_ndjson(const dstd::String& dir, int records) {
    dstd::String out;
    for (int i = 0; i < records; ++i)
        out += common::str_cat(R"({"host":"n)", i % 3, R"(","lat":)", i % 17,
                               ".5", R"(,"op":")", OPS[(i / 400) % 4],
                               R"(","io":{"off":)", i,
                               R"(},"hosts":["h1","h2"],"ok":)",
                               (i % 2 ? "true" : "false"), "}\n");
    const auto plain = dir + "/generic.ndjson";
    {
        dstd::fs::write_file(dstd::PathView(plain), out).discard();
    }
    const auto gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               8 * 1024));
    dstd::fs::remove(plain).discard();
    return gz;
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

index::IndexerOptions options(const dstd::String& dir,
                              dstd::String schema = "") {
    index::IndexerOptions o;
    o.index_dir = dir;
    o.schema = std::move(schema);
    return o;
}

}  // namespace

TEST_SUITE("Schemas") {
    TEST_CASE("the built-in hashes are the ones existing indexes hold") {
        // A change here marks every existing index stale. dftracer's covers
        // its source, whose row sets an older index does not hold.
        CHECK(index::get_schema("dftracer").params_hash() ==
              3999216331681324496ULL);
        CHECK(index::get_schema("generic").params_hash() ==
              7825305221053757732ULL);
    }

    TEST_CASE("detection picks the most specific matching schema") {
        const dstd::Vector<dstd::StringView> dft = {
            R"({"id":1,"name":"read","cat":"POSIX","ph":"X","ts":1})", "[",
            R"({"name":"FH","ph":"M","args":{"name":"/a","value":"1"}})"};
        const dstd::Vector<dstd::StringView> plain = {R"({"op":"read"})",
                                                      R"({"op":"write"})"};
        const dstd::Vector<dstd::StringView> mixed = {
            R"({"name":"read","ph":"X"})", R"({"op":"read"})"};
        CHECK(detect_schema(dft).id == "dftracer");
        CHECK(detect_schema(plain).id == "generic");
        CHECK(detect_schema(mixed).id == "generic");
        CHECK(detect_schema({}).id == "generic");
        CHECK(index::get_schema("generic").require.empty());
        CHECK(index::get_schema("dftracer").params_hash() !=
              index::get_schema("generic").params_hash());
        CHECK_THROWS_AS(index::get_schema("nope"), common::DFTUtilsException);
    }

    TEST_CASE("detection reads the lines the DOM parser refuses") {
        dstd::Vector<dstd::String> owned;
        // The record parser reads a 24-digit integer, kind 2.
        for (int k : {0, 1, 3, 4, 5})
            owned.push_back(dftu_utils_test::refused_line(
                k, R"("id":1,"name":"read","cat":"POSIX","ph":"X","ts":1)"));
        dstd::Vector<dstd::StringView> lines(owned.begin(), owned.end());
        CHECK(detect_schema(lines).id == "dftracer");
    }

    TEST_CASE("a file's schema is detected from its first lines") {
        dftu_utils_test::TestEnvironment env(10);
        CHECK(index::detect_file_schema(env.create_dft_test_gzip_file(50)).id ==
              "dftracer");
        CHECK(index::detect_file_schema(write_ndjson(env.dir(), 50)).id ==
              "generic");
    }

    TEST_CASE("generic NDJSON is indexed and pruned by path") {
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = write_ndjson(env.dir(), 4000);
        const auto dir = env.dir() + "/idx";
        auto ix = index::Indexer::open({dstd::String(dstd::StringView(trace))},
                                       options(dir));
        CHECK(ix.build().indexed == 1);
        const auto files = ix.files();
        REQUIRE(files.size() == 1);
        CHECK(files[0].schema == "generic");

        {
            const auto snapshot =
                common::unwrap(index::plan::open_snapshot(files[0].index_path));
            const auto file =
                snapshot.file(index::store::internal::get_logical_path(trace));
            REQUIRE(file.has_value());
            dstd::Map<dstd::String, index::store::PathStat> cat;
            for (auto& [p, st] :
                 common::unwrap(index::catalog(snapshot, *file)))
                cat.emplace(dstd::String(p.data(), p.size()), st);
            for (const char* p :
                 {"op", "host", "lat", "io.off", "hosts.0", "hosts.1", "ok"})
                CHECK_MESSAGE(cat.count(p), p);
            CHECK(cat.at("op").count == 4000);
            CHECK(cat.at("ok").type == index::store::PathType::Bool);
            const auto blooms = common::unwrap(index::paths(
                snapshot, *file, index::store::IndexExtension::Bloom));
            const auto has = [&](const char* name) {
                return std::any_of(
                    blooms.begin(), blooms.end(), [&](const auto& b) {
                        return dstd::StringView(b.data(), b.size()) == name;
                    });
            };
            CHECK(has("op"));
            CHECK(!has("cat"));
            // Records without a metadata phase: every chunk counts none.
            const auto meta =
                common::unwrap(index::chunk_meta(snapshot, *file));
            REQUIRE(meta.has_value());
            CHECK(!meta->empty());
            for (const auto& [chunk, m] : *meta) CHECK(m.records == 0);
        }

        for (const char* q : {R"(op == "read")", "io.off > 3500",
                              R"(host == "n1" and op == "close")", "lat < 2"}) {
            CAPTURE(q);
            const auto full = count_rows(trace, dir, q, true);
            CHECK(full > 0);
            CHECK(count_rows(trace, dir, q, false) == full);
        }
        const auto explained = ix.explain(R"(op == "read")");
        REQUIRE(explained.size() == 1);
        CHECK(explained[0].read.size() < explained[0].chunks);
    }

    TEST_CASE("a forced schema rebuilds a file indexed under another") {
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = env.create_dft_test_gzip_file(200);
        const auto dir = env.dir() + "/idx";
        index::Indexer::open({dstd::String(dstd::StringView(trace))},
                             options(dir))
            .build();
        CHECK(index::Indexer::open({dstd::String(dstd::StringView(trace))},
                                   options(dir))
                  .files()[0]
                  .schema == "dftracer");
        auto forced = index::Indexer::open(
            {dstd::String(dstd::StringView(trace))}, options(dir, "generic"));
        CHECK(forced.status().needs_work.size() == 1);
        CHECK(forced.build().indexed == 1);
        CHECK(forced.files()[0].schema == "generic");
        // Detection accepts the recorded schema rather than rebuilding.
        CHECK(index::Indexer::open({dstd::String(dstd::StringView(trace))},
                                   options(dir))
                  .status()
                  .needs_work.empty());
        CHECK_THROWS_AS(
            index::Indexer::open({dstd::String(dstd::StringView(trace))},
                                 options(dir, "nope")),
            common::DFTUtilsException);
    }
}
