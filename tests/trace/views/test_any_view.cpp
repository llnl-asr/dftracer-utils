#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "test_view_common.h"

namespace {

constexpr int RECORDS = 3000;

// Records whose `tags` hold 2 to 4 copies of "t<i / 300>", so each run of 300
// records, and so each chunk, holds its own tag; `sizes` holds [i % 7, i].
// Record 2999 also carries "rare" at position 5.
std::string write_tagged(TestEnvironment& env, std::size_t path_budget) {
    const std::string plain = env.get_dir() + "/tagged.ndjson";
    {
        std::ofstream out(plain);
        for (int i = 0; i < RECORDS; ++i) {
            const std::string t = "\"t" + std::to_string(i / 300) + "\"";
            out << R"({"op":"read","tags":[)" << t;
            for (int k = 1; k < 2 + i % 3; ++k) out << "," << t;
            if (i == RECORDS - 1) out << R"(,"x","x","rare")";
            out << R"(],"sizes":[)" << i % 7 << "," << i << "]}\n";
        }
    }
    const std::string gz = plain + ".gz";
    REQUIRE(dftu_utils_test::compress_file_to_gzip_multimember(plain, gz,
                                                               16 * 1024));
    fs::remove(plain);
    dftracer::utils::index::IndexerOptions o;
    o.bloom->path_budget = path_budget;
    dftracer::utils::index::Indexer::open({gz}, o).build();
    return gz;
}

View view_of(const std::string& gz) {
    return View::from_file(gz, determine_index_path(gz, ""));
}

std::size_t rows(const View& v) { return v.collect().get().num_rows(); }

struct NullSink : ExportSink {
    void write(std::string_view) override {}
};

}  // namespace

TEST_SUITE("AnyView") {
    TEST_CASE("any() filters rows and skips chunks without the value") {
        TestEnvironment env(10);
        const auto gz = write_tagged(env, 1024);
        const std::string q = R"(any(tags) == "t7")";
        CHECK(rows(view_of(gz).query(q)) == 300);
        CHECK(rows(view_of(gz).query(R"(not any(tags) == "t7")")) ==
              RECORDS - 300);
        CHECK(rows(view_of(gz).query("any(sizes) >= 2990")) == 10);
        NullSink sink;
        const auto stats = view_of(gz).query(q).sink_json(sink).get();
        CHECK(stats.events_matched == 300);
        CHECK(stats.chunks_skipped > 0);

        auto ix = dftracer::utils::index::Indexer::open({gz});
        const auto e = ix.explain(q);
        REQUIRE(e.size() == 1);
        CHECK(e[0].read.size() < e[0].chunks);
        CHECK(ix.explain(R"(any(tags) == "absent")")[0].read.empty());
    }

    TEST_CASE("an aggregate counts the rows a row scan returns") {
        TestEnvironment env(10);
        const auto gz = write_tagged(env, 1024);
        for (const char* q :
             {R"(any(tags) == "t3")", R"(any(tags) in ["t1", "t2"])",
              "any(sizes) < 1", R"(any(tags) != "t0")"}) {
            CAPTURE(q);
            const auto df = view_of(gz)
                                .query(q)
                                .agg({{AggOp::Count, "", "n"}})
                                .collect()
                                .get();
            const auto& n = df.columns[df.column_index("n")];
            CHECK(static_cast<std::size_t>(n.values<std::int64_t>()[0]) ==
                  rows(view_of(gz).query(q)));
        }
    }

    TEST_CASE("a position past the path budget keeps its chunk") {
        TestEnvironment env(10);
        const auto gz = write_tagged(env, 2);
        CHECK(rows(view_of(gz).query(R"(any(tags) == "rare")")) == 1);
    }
}
