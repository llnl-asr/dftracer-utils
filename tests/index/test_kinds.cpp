#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/index/extensions/chunk_dimension_stats.h>
#include <dftracer/utils/index/extensions/kinds/payloads.h>
#include <dftracer/utils/index/plan/chunk_pruner.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <testing_utilities.h>

#include <string>
#include <vector>

using namespace dftracer::utils;
namespace kinds = dftracer::utils::index::extensions::kinds;
using dftracer::utils::index::extensions::ChunkDimensionStats;
using dftracer::utils::index::store::IndexDatabase;
using dftracer::utils::index::store::IndexExtension;

namespace {

ChunkDimensionStats dim(std::string name, std::vector<std::string> values) {
    ChunkDimensionStats ds;
    ds.dimension = std::move(name);
    ds.value_type = "string";
    for (const auto& v : values) ds.observe(v);
    return ds;
}

// A file of three chunks (two lines each): chunk 0 has cat {a}, chunk 1 has
// cat {b} and chunk 2's cat counts went over the cap (no counts record);
// dur spans [10, 20], [30, 40], [50, 60].
std::string build_index(const std::string& dir, const std::string& trace) {
    const auto path = dir + "/.dftindex";
    IndexDatabase db(path);
    auto w = db.begin_write();
    const int fid = dftu_utils_test::register_test_file(*w, trace, 1);
    const std::vector<std::vector<std::string>> cats = {{"a", "a"}, {"b", "b"}};
    for (std::uint64_t c = 0; c < 3; ++c) {
        dftu_utils_test::index_records::put_gzip_member(
            *w, fid, {c, c * 100, 100, c * 200, 200, c * 2 + 1, c * 2 + 2});
        auto dur = dim("dur", {});
        dur.value_type = "uint";
        dur.min_value = std::to_string(10 + c * 20);
        dur.max_value = std::to_string(20 + c * 20);
        dftu_utils_test::put_dimension_stats(*w, fid, c, dur, 2);
        if (c < 2)
            dftu_utils_test::put_dimension_stats(*w, fid, c,
                                                 dim("cat", cats[c]), 2);
    }
    dftu_utils_test::mark_built(*w, fid);
    w->commit();
    return path;
}

std::vector<std::uint64_t> prune(const std::string& index,
                                 const std::string& trace,
                                 const std::string& text) {
    auto q = duql::Query::from_string(text);
    REQUIRE(q.has_value());
    index::plan::ChunkPruner pruner;
    auto out = pruner({index, trace, std::move(*q)}).get();
    REQUIRE(out.success);
    if (out.candidate_checkpoints.empty() && out.file_may_match)
        return {0, 1, 2};
    return out.candidate_checkpoints;
}

}  // namespace

TEST_SUITE("index kinds") {
    TEST_CASE("payloads round-trip and reject garbage") {
        kinds::Zone zone{"uint", "3", "9", 5, 4, std::nullopt};
        auto z = kinds::decode_zone(kinds::encode_zone(zone));
        REQUIRE(z.has_value());
        CHECK(z->value_type == "uint");
        CHECK(z->min == "3");
        CHECK(z->max == "9");
        CHECK(z->observed == 5);
        CHECK(z->present == 4);
        CHECK_FALSE(z->histogram.has_value());
        CHECK_FALSE(kinds::decode_zone("\x01").has_value());

        kinds::CountsGranule over{7, std::nullopt};
        auto c = kinds::decode_counts(kinds::encode_counts(over));
        REQUIRE(c.has_value());
        CHECK(c->observed == 7);
        CHECK_FALSE(c->compressed.has_value());

        index::extensions::ScalableBloomFilter bf(16, 0.01);
        bf.add("x");
        auto blob = bf.serialize();
        auto b = kinds::decode_bloom(kinds::encode_bloom(blob, 1));
        REQUIRE(b.has_value());
        CHECK(b->possibly_contains("x"));
        CHECK_FALSE(kinds::decode_bloom("short").has_value());
    }

    TEST_CASE("each kind answers only the leaves it can") {
        auto dir = dftu_utils_test::make_unique_test_path("kinds_conditions");
        fs::create_directories(dir);
        const std::string trace = "/fake/kinds.pfw.gz";
        const auto index = build_index(dir.string(), trace);

        // counts: chunk 2 went over the cap, so it is kept.
        CHECK(prune(index, trace, R"(cat == "a")") ==
              std::vector<std::uint64_t>{0, 2});
        CHECK(prune(index, trace, R"(cat in ["b"])") ==
              std::vector<std::uint64_t>{1, 2});
        // zonemap: ranges.
        CHECK(prune(index, trace, "dur > 35") ==
              std::vector<std::uint64_t>{1, 2});
        // NOT: counts prove chunk 0 is all "a"; chunk 2 has no proof.
        CHECK(prune(index, trace, R"(not (cat == "a"))") ==
              std::vector<std::uint64_t>{1, 2});
        // NOT over a range: only chunk 0 lies wholly under 25.
        CHECK(prune(index, trace, "not (dur < 25)") ==
              std::vector<std::uint64_t>{1, 2});
        // A path no kind indexed keeps every chunk.
        CHECK(prune(index, trace, R"(args.other == "z")") ==
              std::vector<std::uint64_t>{0, 1, 2});
    }

    TEST_CASE("a time range keeps a chunk whose events end inside it") {
        auto dir = dftu_utils_test::make_unique_test_path("kinds_time");
        fs::create_directories(dir);
        const std::string trace = "/fake/time.pfw.gz";
        const auto path = dir.string() + "/.dftindex";
        {
            IndexDatabase db(path);
            auto w = db.begin_write();
            const int fid = dftu_utils_test::register_test_file(*w, trace, 1);
            // Both chunks start their events before 50; only chunk 1's last
            // event runs past it.
            const std::pair<int, int> spans[] = {{10, 20}, {10, 100}};
            for (std::uint64_t c = 0; c < 2; ++c) {
                dftu_utils_test::index_records::put_gzip_member(
                    *w, fid,
                    {c, c * 100, 100, c * 200, 200, c * 2 + 1, c * 2 + 2});
                auto te = dim("te", {});
                te.value_type = "uint";
                te.min_value = std::to_string(spans[c].first);
                te.max_value = std::to_string(spans[c].second);
                dftu_utils_test::put_dimension_stats(*w, fid, c, te, 2);
            }
            dftu_utils_test::mark_built(*w, fid);
            w->commit();
        }
        auto result = index::plan::prune_file({path, trace, nullptr, nullptr,
                                               std::make_pair(50.0, 0.0), 2})
                          .get();
        REQUIRE(result.has_value());
        CHECK(result->candidates == std::vector<std::uint64_t>{1});
    }
}
