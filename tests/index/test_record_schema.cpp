#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace dftracer::utils;
namespace store = dftracer::utils::index::store;

namespace {

const char* OPS[] = {"read", "write", "open", "close"};

// Generic NDJSON over small gzip members: `op` is constant for runs of 400
// records, so chunks hold disjoint ops; `io.off` grows with the record.
std::string write_ndjson(const std::string& dir, int records) {
    std::ostringstream out;
    for (int i = 0; i < records; ++i)
        out << R"({"host":"n)" << i % 3 << R"(","lat":)" << i % 17 << ".5"
            << R"(,"op":")" << OPS[(i / 400) % 4] << R"(","io":{"off":)" << i
            << R"(},"hosts":["h1","h2"],"ok":)" << (i % 2 ? "true" : "false")
            << "}\n";
    const auto plain = dir + "/generic.ndjson";
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

index::IndexerOptions options(const std::string& dir, std::string schema = "") {
    index::IndexerOptions o;
    o.index_dir = dir;
    o.schema = std::move(schema);
    return o;
}

}  // namespace

TEST_SUITE("Schemas") {
    TEST_CASE("the built-in hashes are the ones existing indexes hold") {
        // A change here marks every existing index stale.
        CHECK(index::get_schema("dftracer").params_hash() ==
              18021131772592900613ULL);
        CHECK(index::get_schema("generic").params_hash() ==
              7825305221053757732ULL);
    }

    TEST_CASE("detection picks the most specific matching schema") {
        using index::detect_schema;
        const std::vector<std::string_view> dft = {
            R"({"id":1,"name":"read","cat":"POSIX","ph":"X","ts":1})", "[",
            R"({"name":"FH","ph":"M","args":{"name":"/a","value":"1"}})"};
        const std::vector<std::string_view> plain = {R"({"op":"read"})",
                                                     R"({"op":"write"})"};
        const std::vector<std::string_view> mixed = {
            R"({"name":"read","ph":"X"})", R"({"op":"read"})"};
        CHECK(detect_schema(dft).id == "dftracer");
        CHECK(detect_schema(plain).id == "generic");
        CHECK(detect_schema(mixed).id == "generic");
        CHECK(detect_schema({}).id == "generic");
        CHECK(index::get_schema("generic").require.empty());
        CHECK(index::get_schema("dftracer").params_hash() !=
              index::get_schema("generic").params_hash());
        CHECK_THROWS_AS(index::get_schema("nope"), DFTUtilsException);
    }

    TEST_CASE("a file's schema is detected from its first lines") {
        dftu_utils_test::TestEnvironment env(10);
        CHECK(index::detect_file_schema(env.create_dft_test_gzip_file(50)).id ==
              "dftracer");
        CHECK(index::detect_file_schema(write_ndjson(env.get_dir(), 50)).id ==
              "generic");
    }

    TEST_CASE("generic NDJSON is indexed and pruned by path") {
        dftu_utils_test::TestEnvironment env(10);
        const auto trace = write_ndjson(env.get_dir(), 4000);
        const auto dir = env.get_dir() + "/idx";
        auto ix = index::Indexer::open({trace}, options(dir));
        CHECK(ix.build().indexed == 1);
        const auto files = ix.files();
        REQUIRE(files.size() == 1);
        CHECK(files[0].schema == "generic");

        {
            store::IndexDatabase db(files[0].index_path,
                                    store::IndexOpenMode::ReadOnly);
            std::map<std::string, store::PathStat> cat;
            for (auto& [p, st] : db.catalog(static_cast<int>(files[0].file_id)))
                cat.emplace(p, st);
            for (const char* p :
                 {"op", "host", "lat", "io.off", "hosts.0", "hosts.1", "ok"})
                CHECK_MESSAGE(cat.count(p), p);
            CHECK(cat.at("op").count == 4000);
            CHECK(cat.at("ok").type == store::PathType::BOOL);
            const auto blooms =
                db.extension_paths(static_cast<int>(files[0].file_id),
                                   store::IndexExtension::BLOOM);
            CHECK(std::find(blooms.begin(), blooms.end(), "op") !=
                  blooms.end());
            CHECK(std::find(blooms.begin(), blooms.end(), "cat") ==
                  blooms.end());
            // Records without a metadata phase: every chunk counts none.
            auto meta = db.chunk_metadata(static_cast<int>(files[0].file_id));
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
        const auto dir = env.get_dir() + "/idx";
        index::Indexer::open({trace}, options(dir)).build();
        CHECK(index::Indexer::open({trace}, options(dir)).files()[0].schema ==
              "dftracer");
        auto forced = index::Indexer::open({trace}, options(dir, "generic"));
        CHECK(forced.status().needs_work.size() == 1);
        CHECK(forced.build().indexed == 1);
        CHECK(forced.files()[0].schema == "generic");
        // Detection accepts the recorded schema rather than rebuilding.
        CHECK(index::Indexer::open({trace}, options(dir))
                  .status()
                  .needs_work.empty());
        CHECK_THROWS_AS(index::Indexer::open({trace}, options(dir, "nope")),
                        DFTUtilsException);
    }
}
